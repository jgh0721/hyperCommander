Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$Script:Repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$Script:Policy = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'signing-policy.json') -Raw -Encoding UTF8 | ConvertFrom-Json
function Write-UpdateJson([string]$Path, $Value) {
    [IO.Directory]::CreateDirectory((Split-Path $Path -Parent)) | Out-Null
    [IO.File]::WriteAllText($Path, (($Value | ConvertTo-Json -Depth 20) -replace "`r?`n", "`r`n") + "`r`n", [Text.UTF8Encoding]::new($false))
}
function Assert-UpdatePlainPath([string]$Path) {
    $Current = [IO.Path]::GetFullPath($Path)
    while ($Current) {
        if ((Test-Path -LiteralPath $Current) -and ((Get-Item -LiteralPath $Current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Linked update path: $Current" }
        $Parent = [IO.Directory]::GetParent($Current); $Current = if ($Parent) { $Parent.FullName } else { $null }
    }
}
# Path ownership entries are cumulative. A client accepts the union of the
# entries whose since is not newer than its own version (see update-contract.json).
function Get-UpdateOwnership {
    $Contract = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'update-contract.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $Entries = @($Contract.ownership)
    if (-not $Entries.Count) { throw 'The update contract has no path ownership entries.' }
    for ($Index = 1; $Index -lt $Entries.Count; ++$Index) {
        if ([version]$Entries[$Index].since -le [version]$Entries[$Index - 1].since) { throw 'Update ownership entries need increasing versions.' }
    }
    return $Entries
}
function Get-UpdateOwnershipValues($Entry, [string]$Key) {
    $Property = $Entry.PSObject.Properties[$Key]
    if ($Property) { return @($Property.Value) } else { return @() }
}
function Test-UpdateOwnedPath([string]$Path, $Entries) {
    foreach ($Entry in $Entries) {
        if ($Path.Contains('/')) {
            foreach ($Directory in (Get-UpdateOwnershipValues $Entry 'directories')) { if ($Path.StartsWith($Directory, [StringComparison]::Ordinal)) { return $true } }
        } else {
            if ((Get-UpdateOwnershipValues $Entry 'rootFiles') -ccontains $Path) { return $true }
            if ($Path.EndsWith('.dll', [StringComparison]::Ordinal)) {
                foreach ($Prefix in (Get-UpdateOwnershipValues $Entry 'rootDllPrefixes')) { if ($Path.StartsWith($Prefix, [StringComparison]::Ordinal)) { return $true } }
            }
        }
    }
    return $false
}
# The oldest client version whose updater accepts every path in the inventory.
function Get-UpdateMinimumFromVersion($Inventory) {
    $Entries = Get-UpdateOwnership; $Minimum = [version]$Entries[0].since
    foreach ($Name in @(@($Inventory.files | ForEach-Object { [string]$_.path }) + 'product-inventory.json')) {
        $Since = $null
        for ($Count = 1; $Count -le $Entries.Count; ++$Count) {
            if (Test-UpdateOwnedPath $Name $Entries[0..($Count - 1)]) { $Since = [version]$Entries[$Count - 1].since; break }
        }
        if ($null -eq $Since) { throw "Unowned product path: $Name. Add it to update-contract.json ownership." }
        if ($Since -gt $Minimum) { $Minimum = $Since }
    }
    return $Minimum.ToString()
}
# The oldest released updater every package must satisfy (update-contract.json updaterFloor).
# Root files owned only after it ship under rootCarrier and are copied to ROOT by newer code.
function Get-UpdateFloor {
    $Contract = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'update-contract.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $Floor = $Contract.updaterFloor; $Carrier = [string]$Contract.rootCarrier
    if (-not $Floor -or [string]$Floor.version -notmatch '^[0-9]+[.][0-9]+[.][0-9]+[.][0-9]+$' -or $Carrier -notmatch '^([A-Za-z0-9_.+-]+/)+$' -or
        @($Floor.productCertificateSha256).Count -lt 1 -or @($Floor.requiredFiles).Count -lt 1) { throw 'The update contract has no valid updaterFloor/rootCarrier.' }
    $Entries = Get-UpdateOwnership; $Version = [version]$Floor.version
    $Owned = @($Entries | Where-Object { [version]$_.since -le $Version }); $Carried = @()
    if (-not $Owned.Count) { throw 'The updater floor predates the first ownership entry.' }
    foreach ($Entry in @($Entries | Where-Object { [version]$_.since -gt $Version })) {
        foreach ($Key in @('rootDllPrefixes','directories')) {
            if (@(Get-UpdateOwnershipValues $Entry $Key).Count) { throw "Ownership $($Entry.since) adds $Key after the updater floor $Version." }
        }
        $Carried += @(Get-UpdateOwnershipValues $Entry 'rootFiles')
    }
    if (-not (Test-UpdateOwnedPath ($Carrier + 'probe') $Owned)) { throw "The updater floor $Version does not own the root carrier $Carrier." }
    return [pscustomobject]@{version=[string]$Floor.version;ownership=$Owned;productCertificates=@($Floor.productCertificateSha256)
        vendorCertificates=$Floor.vendorCertificates;requiredFiles=@($Floor.requiredFiles);carrier=$Carrier;carried=$Carried}
}
# Fails with every violation when the floor updater would reject the package: an
# unowned path, a signer it does not pin, a missing required file or a misplaced
# carried file. $Signatures maps paths to signer pins already read by the caller.
function Assert-UpdateFloorCompatible([string]$Root, $Inventory, [hashtable]$Signatures = @{}) {
    $Floor = Get-UpdateFloor; $Problems = [Collections.Generic.List[string]]::new()
    $Minimum = Get-UpdateMinimumFromVersion $Inventory
    if ([version]$Minimum -gt [version]$Floor.version) { $Problems.Add("minimumFromVersion $Minimum is newer than the updater floor $($Floor.version)") }
    $Names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($Entry in $Inventory.files) { [void]$Names.Add([string]$Entry.path) }
    foreach ($Entry in $Inventory.files) {
        $Name = [string]$Entry.path
        if (-not (Test-UpdateOwnedPath $Name $Floor.ownership)) {
            if ($Floor.carried -ccontains $Name) { $Problems.Add("${Name}: ship it as $($Floor.carrier)$Name") }
            else { $Problems.Add("${Name}: not owned by the updater floor $($Floor.version)") }
        }
        if ($Name.StartsWith($Floor.carrier, [StringComparison]::OrdinalIgnoreCase)) {
            $Carried = $Name.Substring($Floor.carrier.Length)
            if ($Carried.Contains('/') -or $Floor.carried -cnotcontains $Carried) { $Problems.Add("${Name}: the root carrier holds only carried root files") }
            elseif ($Names.Contains($Carried)) { $Problems.Add("${Name}: also listed at ROOT") }
        }
        $Path = Join-Path $Root $Name
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or -not (Test-UpdatePe $Path)) { continue }
        $Pin = if ($Signatures.ContainsKey($Name)) { $Signatures[$Name] } else { (Get-UpdateSignature $Path).certificateSha256 }
        $Accepted = @($Floor.productCertificates)
        $Vendor = if ($Floor.vendorCertificates) { $Floor.vendorCertificates.PSObject.Properties[$Name] } else { $null }
        if ($Vendor) { $Accepted += @($Vendor.Value) }
        if ($Pin -notin $Accepted) { $Problems.Add("${Name}: signer certificate $Pin is not pinned by the updater floor") }
    }
    foreach ($Required in $Floor.requiredFiles) { if (-not $Names.Contains($Required)) { $Problems.Add("${Required}: required by the updater floor") } }
    if ($Problems.Count) { throw ("Package is not accepted by the updater floor $($Floor.version):`n  " + ($Problems -join "`n  ")) }
}
function Test-UpdatePe([string]$Path) {
    $File = [IO.File]::OpenRead($Path)
    try {
        if ($File.Length -lt 64) { return $false }
        $Reader = [IO.BinaryReader]::new($File)
        if ($Reader.ReadUInt16() -ne 0x5a4d) { return $false }
        $File.Position = 60; $Offset = $Reader.ReadUInt32()
        if ($Offset -gt $File.Length - 6) { throw "Malformed PE: $Path" }
        $File.Position = $Offset
        if ($Reader.ReadUInt32() -ne 0x4550) { throw "Malformed PE: $Path" }
        return $true
    } finally { $File.Dispose() }
}
function Get-UpdateSignature([string]$Path) {
    $Signature = Get-AuthenticodeSignature -LiteralPath $Path
    $Pin = ''; $Cert = $Signature.SignerCertificate
    if ($Cert) {
        $Sha = [Security.Cryptography.SHA256]::Create()
        try { $Pin = [BitConverter]::ToString($Sha.ComputeHash($Cert.RawData)).Replace('-','').ToLowerInvariant() } finally { $Sha.Dispose() }
    }
    return [pscustomobject]@{status=$Signature.Status.ToString();certificateSha256=$Pin;timestampPresent=($null -ne $Signature.TimeStamperCertificate);signatureType=$Signature.SignatureType.ToString()}
}
function Assert-UpdateSignature([string]$Path, [string]$RelativePath = '') {
    $Signature = Get-UpdateSignature $Path
    $Accepted = @($Script:Policy.productCertificateSha256)
    $Property = if ($RelativePath) { $Script:Policy.vendorCertificates.PSObject.Properties[$RelativePath] } else { $null }
    if ($Property) { $Accepted += @($Property.Value) }
    if ($Signature.status -ne 'Valid' -or $Signature.signatureType -ne 'Authenticode' -or
        $Signature.certificateSha256 -notin $Accepted -or ($Script:Policy.requireTimestamp -and -not $Signature.timestampPresent)) { throw "Rejected signature: $RelativePath ($($Signature.status), $($Signature.signatureType))" }
    return $Signature
}
function Invoke-UpdateSigner([string]$Path, [int]$TimeoutSeconds = 180) {
    $Wrapper = Join-Path $Script:Repo 'tools/CertWithEV.cmd'
    if (-not (Test-Path -LiteralPath $Wrapper -PathType Leaf) -or -not (Test-Path -LiteralPath (Join-Path $Script:Repo 'tools/SignotaurTool.exe'))) { throw 'Local code signing tools are missing.' }
    $Text = [IO.File]::ReadAllText($Wrapper)
    $Secrets = @([regex]::Matches($Text, '(?im)^\s*SET\s+(?:API|SERVER)\s*=\s*(.+)$') | ForEach-Object { $_.Groups[1].Value.Trim() })
    $Start = [Diagnostics.ProcessStartInfo]::new()
    $Start.FileName = $env:ComSpec; $Start.Arguments = '/d /s /c ""' + $Wrapper + '" "' + $Path + '""'
    $Start.UseShellExecute = $false; $Start.CreateNoWindow = $true
    $Start.RedirectStandardOutput = $true; $Start.RedirectStandardError = $true
    $Process = [Diagnostics.Process]::new(); $Process.StartInfo = $Start
    try {
        [void]$Process.Start()
        $Out = $Process.StandardOutput.ReadToEndAsync(); $Err = $Process.StandardError.ReadToEndAsync()
        if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
            & taskkill.exe /PID $Process.Id /T /F *> $null
            $Process.WaitForExit(); throw 'Code signing timed out.'
        }
        $Output = $Out.GetAwaiter().GetResult() + "`n" + $Err.GetAwaiter().GetResult()
        foreach ($Value in $Secrets) { if ($Value) { $Output = $Output.Replace($Value,'[REDACTED]'); try { $Output = $Output.Replace(([Uri]$Value).Host,'[REDACTED]') } catch {} } }
        $Output = $Output -replace '(?i)(--api-key\s+)\S+', '$1[REDACTED]'
        # The .cmd can mask a nonzero signer exit. Actual trust is checked separately.
        if ($Process.ExitCode -ne 0) { throw "Code signing failed ($($Process.ExitCode))." }
        return $Output
    } finally { $Process.Dispose() }
}
function Set-UpdateProductSignature([string]$Path, [string]$RelativePath, [string]$CacheRoot) {
    $Path = [IO.Path]::GetFullPath($Path); $CacheRoot = [IO.Path]::GetFullPath($CacheRoot)
    $BuildRoot = Join-Path $Script:Repo '_build'
    foreach ($Target in @($Path,$CacheRoot)) {
        if (-not $Target.StartsWith($BuildRoot + '\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Signing must use a private repository build directory.' }
        Assert-UpdatePlainPath $Target
    }
    if (-not (Test-UpdatePe $Path) -and [IO.Path]::GetExtension($Path) -ine '.cat') { throw "Not a signing input: $RelativePath" }
    $Before = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    $Current = Get-UpdateSignature $Path
    if ($Current.status -eq 'Valid') {
        $Result = Assert-UpdateSignature $Path $RelativePath
        return [pscustomobject]@{path=$RelativePath;sha256=$Before;originalSha256=$Before;signature=$Result;action='preserved'}
    }
    if ($Current.status -ne 'NotSigned') { throw "Refusing to overwrite invalid signature: $RelativePath ($($Current.status))" }
    [IO.Directory]::CreateDirectory($CacheRoot) | Out-Null
    $Key = $Script:Policy.productCertificateSha256[0] + '-' + $Before
    $Mutex = [Threading.Mutex]::new($false,('Local\hc-sign-' + $Key)); $Locked = $false
    try {
        try { $Locked = $Mutex.WaitOne(240000) } catch [Threading.AbandonedMutexException] { $Locked = $true }
        if (-not $Locked) { throw 'Code signing cache lock timed out.' }
        $Cached = Join-Path $CacheRoot ($Key + [IO.Path]::GetExtension($Path))
        if (Test-Path -LiteralPath $Cached) {
            Assert-UpdatePlainPath $Cached; $null = Assert-UpdateSignature $Cached
            Copy-Item -LiteralPath $Cached -Destination $Path -Force; $Action = 'cached'
        } else {
            $Log = Invoke-UpdateSigner $Path; $null = Assert-UpdateSignature $Path
            Copy-Item -LiteralPath $Path -Destination $Cached
            [IO.File]::WriteAllText(($Cached + '.log'),$Log,[Text.UTF8Encoding]::new($false)); $Action = 'signed'
        }
        $Result = Assert-UpdateSignature $Path
        return [pscustomobject]@{path=$RelativePath;sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant();originalSha256=$Before;signature=$Result;action=$Action}
    } finally { if ($Locked) { $Mutex.ReleaseMutex() }; $Mutex.Dispose() }
}
Export-ModuleMember -Function Write-UpdateJson,Assert-UpdatePlainPath,Get-UpdateOwnership,Test-UpdateOwnedPath,Get-UpdateMinimumFromVersion,Get-UpdateFloor,Assert-UpdateFloorCompatible,Test-UpdatePe,Get-UpdateSignature,Assert-UpdateSignature,Set-UpdateProductSignature
