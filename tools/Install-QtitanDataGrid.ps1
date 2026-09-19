param(
    [Parameter(Mandatory = $true)][string]$PackagePath,
    [string]$PrivatePatchPath,
    [string]$RepositoryRoot = (Split-Path $PSScriptRoot -Parent)
)
$ErrorActionPreference = 'Stop'
$Root = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\', '/')
$Manifest = Get-Content -LiteralPath (Join-Path $Root 'tools/qtitan-compatibility.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not $PrivatePatchPath) { $PrivatePatchPath = Join-Path $Root $Manifest.privatePatch.path }
foreach ($InputFile in @(
    @{Path = $PackagePath; Hash = $Manifest.package.sha256},
    @{Path = $PrivatePatchPath; Hash = $Manifest.privatePatch.sha256}
)) {
    if (-not (Test-Path -LiteralPath $InputFile.Path -PathType Leaf)) { throw "Required licensed input is missing: $($InputFile.Path)" }
    if ((Get-FileHash -LiteralPath $InputFile.Path -Algorithm SHA256).Hash -ne $InputFile.Hash) { throw "Licensed input fingerprint differs: $($InputFile.Path)" }
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Expand-CheckedZip([string]$ArchivePath, [string]$Destination) {
    $Destination = [IO.Path]::GetFullPath($Destination).TrimEnd('\', '/')
    $Archive = [IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($ArchivePath))
    try {
        foreach ($Entry in $Archive.Entries) {
            $Target = [IO.Path]::GetFullPath((Join-Path $Destination $Entry.FullName))
            if (-not $Target.StartsWith($Destination + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Archive path escapes staging directory: $($Entry.FullName)" }
            if (-not $Entry.Name) { [IO.Directory]::CreateDirectory($Target) | Out-Null; continue }
            [IO.Directory]::CreateDirectory((Split-Path $Target -Parent)) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($Entry, $Target, $true)
        }
    }
    finally { $Archive.Dispose() }
}
$Stage = Join-Path $Root ('_build/qtitan-install/' + [Guid]::NewGuid().ToString('N'))
$Vendor = Join-Path $Root 'externalLibs/QtitanDataGrid/src'
$Backup = Join-Path $Stage 'previous'
foreach ($Target in @($Stage, $Vendor, $Backup)) {
    if (-not ([IO.Path]::GetFullPath($Target)).StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Install target is outside the repository' }
}
Expand-CheckedZip $PackagePath $Stage
$Prepared = Join-Path $Stage $Manifest.package.directory
Expand-CheckedZip $PrivatePatchPath $Prepared
foreach ($Entry in $Manifest.dependencies) {
    $Relative = $Entry.path.Substring(4) # Manifest paths are relative to the vendor root; prepared is its src directory.
    if (-not $Entry.path.StartsWith('src/')) { throw 'Unexpected compatibility path' }
    $File = Join-Path $Prepared $Relative
    if ((Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash -ne $Entry.sha256) { throw "Prepared source differs: $($Entry.path)" }
}
# Both paths are fixed children of the checked repository. Keep the old installation for rollback.
# A fresh checkout has no ignored commercial component directory yet.
[IO.Directory]::CreateDirectory((Split-Path $Vendor -Parent)) | Out-Null
if (Test-Path -LiteralPath $Vendor) { Move-Item -LiteralPath $Vendor -Destination $Backup }
try { Move-Item -LiteralPath $Prepared -Destination $Vendor }
catch {
    if ((Test-Path -LiteralPath $Backup) -and -not (Test-Path -LiteralPath $Vendor)) { Move-Item -LiteralPath $Backup -Destination $Vendor }
    throw
}
Write-Output "Installed QtitanDataGrid $($Manifest.componentVersion) with private extensions; previous source: $Backup"
& (Join-Path $Root 'tools/Test-QtitanCompatibility.ps1') -RepositoryRoot $Root
