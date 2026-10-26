param(
    [ValidateSet('Debug', 'Release')][string]$Configuration,
    [string]$OutputDirectory,
    [switch]$SkipBuild
)
# Builds HyperCommander and packs the product inventory into a portable ZIP.
# The archive holds one top-level HyperCommander folder; settings are created beside the exe on first run.
# Missing -Configuration / -OutputDirectory are asked interactively.
$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$Kit = 'D:\Qt\Qt-6.12.0-VC2026-X64-H-MD-2026-10-10\bin\qt-cmake.bat'
$Interactive = [Environment]::UserInteractive -and -not [Console]::IsInputRedirected

if (-not $Configuration) {
    if (-not $Interactive) { throw 'Specify -Configuration Debug or Release.' }
    $Choices = [Management.Automation.Host.ChoiceDescription[]]@(
        [Management.Automation.Host.ChoiceDescription]::new('&Release', 'Optimized build for distribution'),
        [Management.Automation.Host.ChoiceDescription]::new('&Debug', 'Debug build with debug Qt runtime'))
    $Configuration = @('Release', 'Debug')[$Host.UI.PromptForChoice('Portable package', 'Select the build configuration', $Choices, 0)]
}
$DefaultOutput = Join-Path $Root '_build\portable'
if (-not $OutputDirectory) {
    if (-not $Interactive) { throw 'Specify -OutputDirectory.' }
    $OutputDirectory = Read-Host "Output directory [$DefaultOutput]"
    if (-not $OutputDirectory.Trim()) { $OutputDirectory = $DefaultOutput }
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory.Trim().Trim('"'))

if (-not $SkipBuild) {
    Push-Location $Root
    try {
        & $Kit --build --preset ('vc2026-' + $Configuration.ToLowerInvariant())
        if ($LASTEXITCODE -ne 0) { throw "$Configuration build failed: $LASTEXITCODE" }
    } finally { Pop-Location }
}

# The build writes the exact product file set and hashes; nothing else is packed.
$Source = Join-Path $Root "_build\vc2026-qt612\$Configuration"
$InventoryPath = Join-Path $Source 'product-inventory.json'
if (-not (Test-Path -LiteralPath $InventoryPath -PathType Leaf)) { throw "Product inventory is missing; build $Configuration first: $InventoryPath" }
$InventoryBytes = [IO.File]::ReadAllBytes($InventoryPath)
$Product = [Text.Encoding]::UTF8.GetString($InventoryBytes) | ConvertFrom-Json
if ($Product.schemaVersion -ne 1 -or $Product.configuration -ne $Configuration -or @($Product.files).Count -lt 10) {
    throw "Invalid $Configuration product inventory: $InventoryPath"
}
if ($Configuration -eq 'Release') {
    # Every Release entry point uses the authenticated update package contract.
    # Private signing runs only in _build, then the four public assets can be
    # exported to the user's chosen directory without copying signing tools.
    $Canonical = Join-Path $Root ('_build/portable-release/' + [Guid]::NewGuid().ToString('N'))
    & (Join-Path $PSScriptRoot 'update/New-PortablePackage.ps1') -Root $Source -OutputRoot $Canonical -Version $Product.version
    Import-Module (Join-Path $PSScriptRoot 'update/ProductSigning.psm1') -Force -DisableNameChecking
    Assert-UpdatePlainPath $OutputDirectory
    $Names = @("HyperCommander-$($Product.version)-windows-x64-portable.zip", "HyperCommander-$($Product.version)-windows-x64-symbols.zip", 'update-manifest.json', 'update-manifest.cat')
    foreach ($Name in $Names) {
        $Target = Join-Path $OutputDirectory $Name; Assert-UpdatePlainPath $Target
        if (Test-Path -LiteralPath $Target) { throw "Release asset already exists; choose a new output directory: $Target" }
    }
    [IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
    foreach ($Name in $Names) { Copy-Item -LiteralPath (Join-Path $Canonical $Name) -Destination (Join-Path $OutputDirectory $Name) }
    Write-Output "Signed Portable Release assets: $OutputDirectory"
    return
}
$Sha = [Security.Cryptography.SHA256]::Create()
function Get-Sha256([IO.Stream]$Stream) { [BitConverter]::ToString($Sha.ComputeHash($Stream)).Replace('-', '') }
$Files = [Collections.Generic.List[object]]::new()
$Seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
try {
    foreach ($Entry in $Product.files) {
        $Relative = [string]$Entry.path
        if ($Relative -match '(^/|^[A-Za-z]:|(^|/)\.\.(/|$))' -or $Relative.Contains('\') -or $Relative -match '(^|/)Qt6Test' -or
            $Entry.sha256 -notmatch '^[A-Fa-f0-9]{64}$' -or -not $Seen.Add($Relative)) { throw "Invalid product inventory entry: $Relative" }
        $Path = Join-Path $Source $Relative
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing product input: $Relative" }
        $Stream = [IO.File]::OpenRead($Path)
        try { $Hash = Get-Sha256 $Stream } finally { $Stream.Dispose() }
        if ($Hash -ne $Entry.sha256.ToUpperInvariant()) { throw "Changed product input (rebuild first): $Relative" }
        $Files.Add(@{path=$Relative; source=$Path; sha256=$Hash})
    }
    $Platform = if ($Configuration -eq 'Debug') { 'Libs/platforms/qwindowsd.dll' } else { 'Libs/platforms/qwindows.dll' }
    foreach ($Required in @('HyperCommander.exe', 'HyperEditor.exe', 'HyperCompare.exe', 'HyperCompareCli.exe', 'hc-plugin-host-x64.exe', $Platform)) {
        if (-not $Seen.Contains($Required)) { throw "Required product input is absent: $Required" }
    }
    $InventoryHash = [BitConverter]::ToString($Sha.ComputeHash($InventoryBytes)).Replace('-', '')
    $Files.Add(@{path='product-inventory.json'; source=$InventoryPath; sha256=$InventoryHash})

    $Revision = ''
    $Commit = & git -C $Root rev-parse --short HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and $Commit) {
        $Revision = '-' + $Commit.Trim()
        if (& git -C $Root status --porcelain --untracked-files=no 2>$null) { $Revision += '-dirty' }
    }
    [IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
    $ZipPath = Join-Path $OutputDirectory ("HyperCommander-$Configuration-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + "$Revision.zip")
    if (Test-Path -LiteralPath $ZipPath) { throw "Output already exists: $ZipPath" }
    # Write to a partial file so an interrupted run never leaves a usable-looking archive.
    $Partial = $ZipPath + '.partial'
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    try {
        $Zip = [IO.Compression.ZipFile]::Open($Partial, [IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($File in $Files) {
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($Zip, $File.source, 'HyperCommander/' + $File.path, [IO.Compression.CompressionLevel]::Optimal)
            }
        } finally { $Zip.Dispose() }
        # Re-read every entry so the archive matches the verified inventory byte for byte.
        $Zip = [IO.Compression.ZipFile]::OpenRead($Partial)
        try {
            if ($Zip.Entries.Count -ne $Files.Count) { throw "Archive holds $($Zip.Entries.Count) entries; expected $($Files.Count)" }
            foreach ($File in $Files) {
                $ZipEntry = $Zip.GetEntry('HyperCommander/' + $File.path)
                if (-not $ZipEntry) { throw "Archive entry missing: $($File.path)" }
                $Stream = $ZipEntry.Open()
                try { $Hash = Get-Sha256 $Stream } finally { $Stream.Dispose() }
                if ($Hash -ne $File.sha256) { throw "Archive entry differs: $($File.path)" }
            }
        } finally { $Zip.Dispose() }
        Move-Item -LiteralPath $Partial -Destination $ZipPath
    } catch {
        if (Test-Path -LiteralPath $Partial) { Remove-Item -LiteralPath $Partial -Force }
        throw
    }
} finally { $Sha.Dispose() }
$Size = (Get-Item -LiteralPath $ZipPath).Length / 1MB
Write-Output ("Portable {0} package: {1} files, {2:N1} MB" -f $Configuration, $Files.Count, $Size)
Write-Output "Archive: $ZipPath"
Write-Output 'Extract anywhere and run HyperCommander\HyperCommander.exe; settings are stored in that folder.'
