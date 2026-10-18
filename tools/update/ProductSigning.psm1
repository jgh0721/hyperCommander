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
Export-ModuleMember -Function Write-UpdateJson,Assert-UpdatePlainPath,Test-UpdatePe,Get-UpdateSignature,Assert-UpdateSignature,Set-UpdateProductSignature
