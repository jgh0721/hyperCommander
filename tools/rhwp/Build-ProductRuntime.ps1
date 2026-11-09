param(
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [switch]$ApproveReadOnly,
    [switch]$ApproveEditor,
    [string]$EngineRoot,
    [string]$SourceRoot
)
$ErrorActionPreference = 'Stop'
if ($ApproveReadOnly -eq $ApproveEditor) { throw 'Select exactly one explicit read-only or editor release profile.' }
$Repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $OutputRoot) { throw 'Output must be a new directory.' }
if (-not $EngineRoot) { $EngineRoot = Join-Path $Repo '_build/rhwp-h2/engine' }
if (-not $SourceRoot) { $SourceRoot = Join-Path $Repo '_build/rhwp-h0/source' }
$QtCMake = 'D:/Qt/Qt-6.12.0-VC2026-X64-H-MD-2026-10-10/bin/qt-cmake.bat'
$Encoding = [Text.UTF8Encoding]::new($false)
function SaveJson($Path, $Value) {
    $Text = ($Value | ConvertTo-Json -Depth 20) -replace "`r?`n", "`r`n"
    [IO.File]::WriteAllText($Path, $Text + "`r`n", $Encoding)
}
function RunCMake([string[]]$Arguments, [string]$Log) {
    # Windows PowerShell turns native stderr warnings into ErrorRecords.
    # Treat the native exit status, not a successful CMake warning, as failure.
    $ErrorActionPreference = 'Continue'
    & $QtCMake @Arguments *> $Log
    $Result = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($Result -ne 0) { throw "Qt CMake failed; see $Log" }
}
New-Item -ItemType Directory -Path $OutputRoot | Out-Null
Push-Location $Repo
try {
    $Bundle = Join-Path $OutputRoot 'bundle'
    $Builder = if ($ApproveEditor) { 'tools/rhwp/build-editor-product-bundle.mjs' } else { 'tools/rhwp/build-viewer-bundle.mjs' }
    $ErrorActionPreference = 'Continue'
    & node $Builder $EngineRoot $SourceRoot $Bundle
    $NodeExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($NodeExit -ne 0) { throw 'Pinned bundle creation failed.' }
    $ManifestPath = Join-Path $Bundle 'asset-manifest.json'
    $Manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $Manifest.releaseEligible = $true
    SaveJson $ManifestPath $Manifest
    $ManifestPin = (Get-FileHash -LiteralPath $ManifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
    RunCMake @('--preset','vc2026-qt612','-DHC_ENABLE_RHWP=ON',"-DHC_RHWP_BUNDLE_DIRECTORY=$Bundle",
        "-DHC_RHWP_APPROVED_MANIFEST_SHA256=$ManifestPin",'-DHC_RHWP_PRODUCT_RUNTIME_DIRECTORY=',
        '-DHC_RHWP_APPROVED_INVENTORY_SHA256=') (Join-Path $OutputRoot 'helper-configure.log')
    RunCMake @('--build','--preset','vc2026-release','--target','HyperEditorWeb') (Join-Path $OutputRoot 'helper-build.log')
    $Runtime = Join-Path $OutputRoot 'runtime'
    & node tools/rhwp/stage-runtime.mjs (Join-Path $Repo '_build/vc2026-qt612/rhwp-runtime/Release') $Bundle $Runtime qt-resource
    if ($LASTEXITCODE -ne 0) { throw 'Resource-verified runtime staging failed.' }
    $InventoryPath = Join-Path $Runtime 'runtime-inventory.json'
    $Inventory = Get-Content -LiteralPath $InventoryPath -Raw -Encoding UTF8 | ConvertFrom-Json
    # Seal the signed bytes: product deployment shares these Qt DLLs with the host.
    # Signing after sealing would invalidate the runtime inventory on the first build.
    Import-Module (Join-Path $Repo 'tools/update/ProductSigning.psm1') -Force -DisableNameChecking
    foreach ($Entry in $Inventory.files) {
        $InputPath = Join-Path $Runtime $Entry.path
        if (Test-UpdatePe $InputPath) {
            $null = Set-UpdateProductSignature $InputPath $Entry.path (Join-Path $Repo '_build/signing-cache')
            $Entry.sha256 = (Get-FileHash -LiteralPath $InputPath -Algorithm SHA256).Hash.ToLowerInvariant()
            $Entry.bytes = (Get-Item -LiteralPath $InputPath).Length
        }
    }
    $Inventory.releaseEligible = $true
    SaveJson $InventoryPath $Inventory
    $InventoryPin = (Get-FileHash -LiteralPath $InventoryPath -Algorithm SHA256).Hash.ToLowerInvariant()
    & (Join-Path $PSScriptRoot 'Test-ProductRuntime.ps1') -RuntimeRoot $Runtime -ExpectedSha256 $InventoryPin -AsJson |
        Set-Content -LiteralPath (Join-Path $OutputRoot 'runtime-validation.json') -Encoding UTF8
    RunCMake @('--preset','vc2026-qt612','-DHC_ENABLE_RHWP=ON',"-DHC_RHWP_BUNDLE_DIRECTORY=$Bundle",
        "-DHC_RHWP_APPROVED_MANIFEST_SHA256=$ManifestPin", "-DHC_RHWP_PRODUCT_RUNTIME_DIRECTORY=$Runtime",
        "-DHC_RHWP_APPROVED_INVENTORY_SHA256=$InventoryPin") (Join-Path $OutputRoot 'product-configure.log')
    SaveJson (Join-Path $OutputRoot 'release.json') @{schemaVersion=1; profile=$Manifest.profile;
        runtime=$Runtime; manifestSha256=$ManifestPin; inventorySha256=$InventoryPin; editing=[bool]$ApproveEditor; f3=$true}
    Write-Output "Product runtime configured: $Runtime"
} finally { Pop-Location }
