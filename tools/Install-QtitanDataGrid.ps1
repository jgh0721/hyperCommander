param(
    [string]$RepositoryRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$BuildDirectory,
    [string]$Preset = 'vc2026-qt612',
    [switch]$VerifyOnly,
    [string]$GeneratedRoot,
    [string]$ManifestPath
)
$ErrorActionPreference = 'Stop'
$Root = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\', '/')
if (-not $ManifestPath) { $ManifestPath = Join-Path $Root 'tools/qtitan-compatibility.json' }
$Manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
$Vendor = Join-Path $Root $Manifest.integrated.submodule
$ContractPath = Join-Path $Vendor $Manifest.integrated.sourceManifest
if (-not (Test-Path -LiteralPath $ContractPath -PathType Leaf)) {
    throw 'Initialize the licensed nested submodules first: git submodule update --init --recursive'
}
$Contract = Get-Content -LiteralPath $ContractPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($Contract.upstreamVersion -ne $Manifest.integrated.upstreamVersion -or $Contract.baseVersion -ne $Manifest.integrated.baseVersion) {
    throw 'The private source contract does not match the product integration version.'
}
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $Root '_build/vc2026-qt612' }
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $Root $BuildDirectory }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\', '/')
if (-not $GeneratedRoot) { $GeneratedRoot = Join-Path $BuildDirectory 'watercolor/third_party/QtitanDataGrid/src' }
if (-not $VerifyOnly) {
    if (-not $BuildDirectory.StartsWith((Join-Path $Root '_build') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Use a build directory under the repository _build directory.'
    }
    if ([IO.Path]::GetFullPath($GeneratedRoot) -ne [IO.Path]::GetFullPath((Join-Path $BuildDirectory 'watercolor/third_party/QtitanDataGrid/src'))) {
        throw 'A custom GeneratedRoot is only supported with VerifyOnly.'
    }
    # Configure generates the source from the private upstream tree and ordered
    # patches. Never modify the submodule checkout or copy a prepatched work tree.
    $Kit = 'D:\Qt\Qt-6.12.0-VC2026-X64-H-MD-2026-09-04\bin\qt-cmake.bat'
    if (-not (Test-Path -LiteralPath $Kit -PathType Leaf)) { throw "The prescribed Qt kit is missing: $Kit" }
    Push-Location $Root
    try {
        # Windows PowerShell reports native stderr warnings as error records.
        # CMake's exit code, not the presence of a warning, determines success.
        $PreviousPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            & $Kit --preset $Preset -B $BuildDirectory -U 'HC_QTITAN_*'
            $Result = $LASTEXITCODE
        } finally { $ErrorActionPreference = $PreviousPreference }
        if ($Result -ne 0) { throw "Qtitan source generation failed: $Result" }
    } finally { Pop-Location }
}
$Entries = @($Contract.generatedSources.PSObject.Properties)
if ($Entries.Count -eq 0) { throw 'The private source contract is empty.' }
foreach ($Entry in $Entries) {
    $File = Join-Path $GeneratedRoot $Entry.Name
    if (-not (Test-Path -LiteralPath $File -PathType Leaf) -or (Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash -ne $Entry.Value) {
        throw "Integrated Qtitan source mismatch: $($Entry.Name)"
    }
}
foreach ($Entry in $Manifest.integrated.requiredApis.PSObject.Properties) {
    $Text = Get-Content -LiteralPath (Join-Path $GeneratedRoot $Entry.Name) -Raw -Encoding UTF8
    foreach ($Api in $Entry.Value) {
        if (-not $Text.Contains($Api)) { throw "Missing integrated Qtitan API: $Api" }
    }
}
Write-Output "Integrated Qtitan $($Contract.upstreamVersion): $($Entries.Count) generated sources and required APIs match."
