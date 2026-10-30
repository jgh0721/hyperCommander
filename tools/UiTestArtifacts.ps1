# Only directories created with our ownership marker are eligible for collection.
function Remove-UiArtifactFile([string]$Path) {
    # Clearing ReadOnly on a hardlink also changes every live Qt snapshot link.
    # Delete this directory entry without changing the shared file attributes.
    if (-not ('UiArtifactNative' -as [type])) {
        Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class UiArtifactNative {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr CreateFile(string p, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool SetFileInformationByHandle(IntPtr h, int c, ref uint flags, uint size);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
'@
    }
    $Handle = [UiArtifactNative]::CreateFile($Path, 0x10000, 7, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
    if ($Handle -eq [IntPtr](-1)) { throw "Cannot open artifact for deletion: $Path" }
    try {
        [uint32]$Flags = 0x13 # DELETE | POSIX_SEMANTICS | IGNORE_READONLY_ATTRIBUTE
        if (-not [UiArtifactNative]::SetFileInformationByHandle($Handle, 21, [ref]$Flags, 4)) {
            throw "Cannot delete artifact: $Path (Win32 $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
        }
    } finally { [void][UiArtifactNative]::CloseHandle($Handle) }
}

function Clear-UiArtifactRun([string]$Base, [string]$Directory, [bool]$KeepEvidence) {
    $Base = [IO.Path]::GetFullPath($Base).TrimEnd('\')
    $Directory = [IO.Path]::GetFullPath($Directory)
    if ([IO.Path]::GetDirectoryName($Directory) -ine $Base -or
        [IO.Path]::GetFileName($Directory) -notmatch '^app-[0-9a-f]{32}$') { throw 'Invalid UI artifact path' }
    $RootItem = Get-Item -LiteralPath $Directory -Force
    if ($RootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'UI artifact root is a link' }
    # Walk one level at a time; never enumerate through junctions/symlinks.
    $Pending = [Collections.Generic.Stack[string]]::new(); $Pending.Push($Directory)
    $Files = @(); $Directories = @()
    while ($Pending.Count) {
        foreach ($Item in Get-ChildItem -LiteralPath $Pending.Pop() -Force -ErrorAction Stop) {
            if ($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "UI artifact contains a link: $($Item.FullName)" }
            if ($Item.PSIsContainer) { $Directories += $Item; $Pending.Push($Item.FullName) }
            else { $Files += $Item }
        }
    }
    [long]$EvidenceBytes = 0
    foreach ($File in $Files | Sort-Object FullName) {
        if ($File.Name -in @('.ui-artifacts.json', '.ui-artifacts.lock')) { continue }
        $Keep = $KeepEvidence -and ($File.Name -in @('result.txt','result.xml','stderr.txt','failure.txt') -or $File.Extension -in @('.log','.png','.dmp'))
        if ($Keep -and ($EvidenceBytes + $File.Length) -le 64MB) { $EvidenceBytes += $File.Length; continue }
        Remove-UiArtifactFile $File.FullName
    }
    foreach ($Dir in $Directories | Sort-Object { $_.FullName.Length } -Descending) {
        if (-not @(Get-ChildItem -LiteralPath $Dir.FullName -Force).Count) { [IO.Directory]::Delete($Dir.FullName) }
    }
}

function Invoke-UiArtifactCollection([string]$Base) {
    if (-not [IO.Directory]::Exists($Base)) { return }
    foreach ($Directory in Get-ChildItem -LiteralPath $Base -Directory -Filter 'app-*') {
        $Lease = $null
        try {
            if ($Directory.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
            $Marker = Join-Path $Directory.FullName '.ui-artifacts.json'
            if (-not [IO.File]::Exists($Marker)) { continue } # Legacy/other session data.
            foreach ($Control in @($Marker, (Join-Path $Directory.FullName '.ui-artifacts.lock'))) {
                if ((Get-Item -LiteralPath $Control -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Artifact control file is a link' }
            }
            $Lease = [IO.File]::Open((Join-Path $Directory.FullName '.ui-artifacts.lock'), 'Open', 'ReadWrite', 'None')
            $State = Get-Content -LiteralPath $Marker -Raw | ConvertFrom-Json
            if ($State.schema -ne 'hc-ui-artifacts-v1') { continue }
            if ($State.status -ne 'running' -and [datetime]$State.expiresUtc -gt [datetime]::UtcNow) { continue }
            $Owner = Get-Process -Id $State.ownerPid -ErrorAction SilentlyContinue
            if ($Owner -and $Owner.StartTime.ToUniversalTime().ToString('o') -eq $State.ownerStartUtc) { continue }
            $Active = @(Get-Process | Where-Object { $_.Path -and $_.Path.StartsWith($Directory.FullName + '\', [StringComparison]::OrdinalIgnoreCase) })
            if ($Active.Count) { continue }
            # Interrupted runs retain bounded evidence for seven days as failures.
            $Interrupted = $State.status -eq 'running'
            Clear-UiArtifactRun $Base $Directory.FullName $Interrupted
            if ($Interrupted) {
                $State.status = 'interrupted'; $State.expiresUtc = [datetime]::UtcNow.AddDays(7).ToString('o')
                $State | ConvertTo-Json | Set-Content -LiteralPath $Marker -Encoding UTF8
            } else {
                Remove-UiArtifactFile $Marker
                $Lease.Dispose(); $Lease = $null
                Remove-UiArtifactFile (Join-Path $Directory.FullName '.ui-artifacts.lock')
                [IO.Directory]::Delete($Directory.FullName)
            }
        } catch { Write-Verbose "UI artifact collection deferred: $($_.Exception.Message)" }
        finally { if ($Lease) { $Lease.Dispose() } }
    }
}

function Start-UiArtifactRun([string]$Directory, [string]$Purpose) {
    [IO.Directory]::CreateDirectory($Directory) | Out-Null
    $Lease = [IO.File]::Open((Join-Path $Directory '.ui-artifacts.lock'), 'CreateNew', 'ReadWrite', 'None')
    try {
        $State = [ordered]@{ schema='hc-ui-artifacts-v1'; ownerPid=$PID; ownerStartUtc=(Get-Process -Id $PID).StartTime.ToUniversalTime().ToString('o'); purpose=$Purpose; createdUtc=[datetime]::UtcNow.ToString('o'); expiresUtc=[datetime]::UtcNow.AddDays(7).ToString('o'); status='running' }
        $State | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Directory '.ui-artifacts.json') -Encoding UTF8
        return $Lease
    } catch { $Lease.Dispose(); throw }
}

function Complete-UiArtifactRun([string]$Directory, [IO.FileStream]$Lease, [bool]$Succeeded, [string]$KeepReason) {
    try {
        $Marker = Join-Path $Directory '.ui-artifacts.json'
        $State = Get-Content -LiteralPath $Marker -Raw | ConvertFrom-Json
        $State.status = if ($Succeeded) { 'success' } else { 'failed' }
        $State.expiresUtc = [datetime]::UtcNow.AddDays(7).ToString('o')
        if ($KeepReason) { $State.purpose += '; preserved: ' + $KeepReason }
        $State | ConvertTo-Json | Set-Content -LiteralPath $Marker -Encoding UTF8
        $Active = @(Get-Process | Where-Object { $_.Path -and $_.Path.StartsWith($Directory + '\', [StringComparison]::OrdinalIgnoreCase) })
        if ($Active.Count) { throw 'A child process still uses this runtime; retaining it for later collection' }
        if (-not $KeepReason) { Clear-UiArtifactRun (Split-Path $Directory -Parent) $Directory (-not $Succeeded) }
        if ($Succeeded -and -not $KeepReason) {
            Remove-UiArtifactFile $Marker
            $Lease.Dispose(); $Lease = $null
            Remove-UiArtifactFile (Join-Path $Directory '.ui-artifacts.lock')
            [IO.Directory]::Delete($Directory)
            Write-Output 'Local UI temporary runtime removed.'
        } else { Write-Output "Local UI evidence (7-day retention): $Directory" }
    } catch { Write-Warning "UI artifact cleanup deferred: $Directory : $($_.Exception.Message)" }
    finally { if ($Lease) { $Lease.Dispose() } }
}
