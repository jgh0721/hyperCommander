param(
    [string]$Destination = 'D:\Utils\HyperCommander',
    [switch]$SkipBuild,
    [string]$SourceDirectory
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$Kit = 'D:\Qt\Qt-6.12.0-VC2026-X64-H-MD-2026-10-10\bin\qt-cmake.bat'
if (-not $SkipBuild) {
    Push-Location $Root
    try {
        & $Kit --build --preset vc2026-release
        if ($LASTEXITCODE -ne 0) { throw "Release build failed: $LASTEXITCODE" }
    } finally { Pop-Location }
}
$Source = if ($SourceDirectory) { [IO.Path]::GetFullPath($SourceDirectory) } else { Join-Path $Root '_build/vc2026-qt612/Release' }
$Destination = [IO.Path]::GetFullPath($Destination).TrimEnd('\', '/')
if ($Destination -eq [IO.Path]::GetPathRoot($Destination).TrimEnd('\') -or
    $Destination.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase) -or $Destination -eq $Root) {
    throw 'Installation must use a dedicated directory outside the source tree.'
}
foreach ($Process in Get-Process) {
    if ($Process.Path -and $Process.Path.StartsWith($Destination + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Close the installed application before updating: $($Process.ProcessName) ($($Process.Id))"
    }
}
# CMake and this updater use the same explicit product inventory. Validate all
# source hashes before the first write so missing/tampered inputs cannot update
# a subset of a working installation.
$InventoryPath = Join-Path $Source 'product-inventory.json'
$InventoryBytes = [IO.File]::ReadAllBytes($InventoryPath)
$Product = [Text.Encoding]::UTF8.GetString($InventoryBytes) | ConvertFrom-Json
if ($Product.schemaVersion -ne 1 -or $Product.configuration -ne 'Release' -or @($Product.files).Count -lt 10) {
    throw 'Invalid Release product inventory.'
}
# The RHWP runtime shares the product ROOT; only its own folders are sealed against unlisted files.
$RhwpPins = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
if ($Product.rhwpInventorySha256) {
    $Rhwp = & (Join-Path $PSScriptRoot 'rhwp/Test-ProductRuntime.ps1') -RuntimeRoot $Source -SharedRoot -ExpectedSha256 $Product.rhwpInventorySha256 -DestinationRoot $Destination
    foreach ($Entry in $Rhwp.files) { $RhwpPins.Add($Entry.path, $Entry.sha256) }
}
$EpubPins = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
if ($Product.epubInventorySha256) {
    $Epub = & (Join-Path $PSScriptRoot 'epub/Test-ProductRuntime.ps1') -RuntimeRoot (Join-Path $Source 'Libs/Epub') -ExpectedSha256 $Product.epubInventorySha256
    foreach ($Entry in $Epub.files) { $EpubPins.Add(('Libs/Epub/' + $Entry.path), $Entry.sha256) }
}
# Files of the previous installation, used to remove what the new layout no longer ships.
$PreviousInventory = Join-Path $Destination 'product-inventory.json'
$Previous = if (Test-Path -LiteralPath $PreviousInventory) { Get-Content -LiteralPath $PreviousInventory -Raw -Encoding UTF8 | ConvertFrom-Json } else { $null }
$Files = [Collections.Generic.List[string]]::new()
$Hashes = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($Entry in $Product.files) {
    $Relative = [string]$Entry.path
    if ($Relative -match '(^/|^[A-Za-z]:|(^|/)\.\.(/|$))' -or $Relative.Contains('\') -or
        ($Relative -notmatch '^(HyperCommander(?:Admin|Updater)?\.exe|HyperEditor\.exe|HyperCompare(?:Cli)?\.exe|hc-(?:plugin-host-x64|plugin-inspect|search-index)\.exe|Everything64\.dll|(?:Qt6|vcruntime|msvcp|concrt|d3dcompiler_|opengl32sw|pdfium)[A-Za-z0-9_.]*\.dll|Libs/(?:platforms|imageformats|iconengines|styles|generic|networkinformation|tls|sqldrivers|printsupport)/[A-Za-z0-9_.-]+\.dll|Libs/translations/[A-Za-z0-9_.-]+\.qm|Libs/Fonts/D2Coding\.rcc|Licenses/[^:]+|Addons/WCX/IsoImage/[^:]+|Addons/WLX/HyperViewer/[^:]+|Third-Party/(?:SevenZip|AlzEgg)/[^:]+)$' -and -not $RhwpPins.ContainsKey($Relative) -and -not $EpubPins.ContainsKey($Relative)) -or
        $Relative -match '(^|/)Qt6Test' -or $Entry.sha256 -notmatch '^[A-Fa-f0-9]{64}$' -or $Hashes.ContainsKey($Relative)) {
        throw "Invalid product inventory entry: $Relative"
    }
    $InputFile = Join-Path $Source $Relative
    if (-not (Test-Path -LiteralPath $InputFile -PathType Leaf) -or (Get-FileHash -LiteralPath $InputFile -Algorithm SHA256).Hash -ne $Entry.sha256) {
        throw "Missing or changed product input: $Relative"
    }
    $Files.Add($Relative); $Hashes.Add($Relative, $Entry.sha256)
}
$ViewerPackage = 'Addons/WLX/HyperViewer/'
foreach ($Required in @('HyperCommander.exe','HyperEditor.exe','HyperCompare.exe','HyperCompareCli.exe','Libs/platforms/qwindows.dll','pdfium.dll',($ViewerPackage + 'hyperviewer.json'),($ViewerPackage + 'source-lock.json'))) {
    if (-not $Hashes.ContainsKey($Required)) { throw "Required product input is absent: $Required" }
}
foreach ($Name in $RhwpPins.Keys) {
    if (-not $Hashes.ContainsKey($Name) -or $Hashes[$Name] -ine $RhwpPins[$Name]) { throw "RHWP product inventory mismatch: $Name" }
}
foreach ($Name in $EpubPins.Keys) {
    if (-not $Hashes.ContainsKey($Name) -or $Hashes[$Name] -ine $EpubPins[$Name]) { throw "EPUB product inventory mismatch: $Name" }
}
# The viewer manifest sits in its package folder; its binary and dependencies are ROOT-relative.
$ViewerManifest = Get-Content -LiteralPath (Join-Path $Source ($ViewerPackage + 'hyperviewer.json')) -Raw | ConvertFrom-Json
$ViewerHelpers = @()
if ($ViewerManifest.PSObject.Properties.Name -contains 'helpers') { $ViewerHelpers = @($ViewerManifest.helpers.PSObject.Properties) }
if ($ViewerManifest.id -ne 'builtin.hyperviewer' -or $ViewerManifest.binary -ne ($ViewerPackage + 'HyperViewer.dll') -or
    $ViewerHelpers.Count -ne 0) { throw 'Unexpected HyperViewer package identity.' }
$Pinned = @{ $ViewerManifest.binary = $ViewerManifest.sha256 }
foreach ($Property in $ViewerHelpers + @($ViewerManifest.dependencies.PSObject.Properties)) { $Pinned[$Property.Name] = $Property.Value }
foreach ($Name in $Pinned.Keys) {
    if (-not $Hashes.ContainsKey($Name) -or $Hashes[$Name] -ne $Pinned[$Name]) { throw "HyperViewer manifest hash mismatch: $Name" }
}
& (Join-Path $PSScriptRoot 'update/Test-ProductSignatures.ps1') -Root $Source
$Sha = [Security.Cryptography.SHA256]::Create()
try { $InventoryHash = [BitConverter]::ToString($Sha.ComputeHash($InventoryBytes)).Replace('-','') } finally { $Sha.Dispose() }
$Files.Add('product-inventory.json'); $Hashes.Add('product-inventory.json', $InventoryHash)
$Evidence = Join-Path $Root ('_build/install/' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($Evidence) | Out-Null
$Changed = [Collections.Generic.List[object]]::new()
$Inventory = [Collections.Generic.List[object]]::new()
try {
    foreach ($Relative in $Files) {
        $InputFile = Join-Path $Source $Relative
        $Target = Join-Path $Destination $Relative
        $Hash = $Hashes[$Relative]
        if ((Get-FileHash -LiteralPath $InputFile -Algorithm SHA256).Hash -ne $Hash) { throw "Source changed during installation: $Relative" }
        $Existed = Test-Path -LiteralPath $Target
        if (-not $Existed -or (Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash -ne $Hash) {
            $Backup = Join-Path $Evidence ('previous/' + $Relative)
            if ($Existed) {
                [IO.Directory]::CreateDirectory((Split-Path $Backup -Parent)) | Out-Null
                Copy-Item -LiteralPath $Target -Destination $Backup
            }
            $Changed.Add(@{target=$Target; backup=$Backup; existed=$Existed})
            [IO.Directory]::CreateDirectory((Split-Path $Target -Parent)) | Out-Null
            Copy-Item -LiteralPath $InputFile -Destination $Target -Force
        }
        if ((Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash -ne $Hash) { throw "Installed hash mismatch: $Relative" }
        $Inventory.Add(@{path=$Relative; sha256=$Hash})
    }
} catch {
    foreach ($Change in $Changed) {
        if ($Change.existed) { Copy-Item -LiteralPath $Change.backup -Destination $Change.target -Force }
        elseif (Test-Path -LiteralPath $Change.target) { Remove-Item -LiteralPath $Change.target }
    }
    throw
}
# Remove what the previous installation shipped and this one does not (e.g. an older directory layout).
# Only files still matching their recorded hash are removed; user-modified files and user files stay.
$Removed = [Collections.Generic.List[string]]::new()
if ($Previous -and $Previous.files) {
    $Folders = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($Entry in $Previous.files) {
        $Relative = [string]$Entry.path
        if ($Hashes.ContainsKey($Relative) -or $Relative -match '(^/|^[A-Za-z]:|(^|/)\.\.(/|$))' -or $Relative.Contains('\')) { continue }
        $Target = Join-Path $Destination $Relative
        if (-not (Test-Path -LiteralPath $Target -PathType Leaf)) { continue }
        if ((Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash -ne $Entry.sha256) { Write-Warning "Kept modified file: $Relative"; continue }
        try {
            $Backup = Join-Path $Evidence ('removed/' + $Relative)
            [IO.Directory]::CreateDirectory((Split-Path $Backup -Parent)) | Out-Null
            Copy-Item -LiteralPath $Target -Destination $Backup
            Remove-Item -LiteralPath $Target
            $Removed.Add($Relative)
            for ($Folder = Split-Path $Relative -Parent; $Folder; $Folder = Split-Path $Folder -Parent) { [void]$Folders.Add($Folder) }
        } catch { Write-Warning "Could not remove obsolete file: $Relative ($($_.Exception.Message))" }
    }
    # Deepest first, so emptied parents can follow; non-empty folders hold user files and stay.
    foreach ($Folder in ($Folders | Sort-Object { $_.Split([char[]]'\/').Count } -Descending)) {
        $Path = Join-Path $Destination $Folder
        if ((Test-Path -LiteralPath $Path) -and -not (Get-ChildItem -LiteralPath $Path -Force)) { Remove-Item -LiteralPath $Path }
    }
}
$Report = @{destination=$Destination; configuration='Release'; utc=[DateTime]::UtcNow.ToString('o'); files=$Inventory.ToArray(); removed=$Removed.ToArray()}
[IO.File]::WriteAllText((Join-Path $Evidence 'installed.json'), (($Report | ConvertTo-Json -Depth 5) -replace "`r?`n", "`r`n"), [Text.UTF8Encoding]::new($false))
Write-Output "Installed $($Files.Count) product files: $Destination"
if ($Removed.Count) { Write-Output "Removed $($Removed.Count) files the previous installation shipped" }
Write-Output "Verified SHA256 inventory and overwritten-file backups: $Evidence"
