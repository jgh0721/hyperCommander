param([Parameter(Mandatory=$true)][string]$Root,[string]$CacheRoot)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ProductSigning.psm1') -Force -DisableNameChecking
$Root = [IO.Path]::GetFullPath($Root)
if (-not $CacheRoot) { $CacheRoot = Join-Path $PSScriptRoot '../../_build/signing-cache' }
Assert-UpdatePlainPath $Root
$Names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($Name in @('HyperCommander.exe','HyperCommanderAdmin.exe','HyperEditor.exe','HyperCompare.exe','HyperCompareCli.exe','HyperEditorWeb.exe','QtWebEngineProcess.exe','hc-plugin-host-x64.exe','hc-plugin-inspect.exe','hc-search-index.exe','HyperCommanderUpdater.exe','Everything64.dll')) {
    if (Test-Path -LiteralPath (Join-Path $Root $Name) -PathType Leaf) { [void]$Names.Add($Name) }
}
foreach ($File in Get-ChildItem -LiteralPath $Root -File) {
    if ($File.Name -match '^(Qt6|vcruntime|msvcp|concrt|d3dcompiler_|opengl32sw|pdfium)[A-Za-z0-9_.]*\.dll$' -and $File.Name -notmatch '^Qt6Test') { [void]$Names.Add($File.Name) }
}
foreach ($Folder in @('Libs/platforms','Libs/imageformats','Libs/iconengines','Libs/styles','Libs/generic','Libs/networkinformation','Libs/tls','Libs/sqldrivers','Libs/printsupport','Addons/WCX/IsoImage','Addons/WLX/HyperViewer','Third-Party/SevenZip','Third-Party/AlzEgg')) {
    $Directory = Join-Path $Root $Folder
    if (-not (Test-Path -LiteralPath $Directory)) { continue }
    Assert-UpdatePlainPath $Directory
    foreach ($File in Get-ChildItem -LiteralPath $Directory -File) {
        Assert-UpdatePlainPath $File.FullName
        if (Test-UpdatePe $File.FullName) { [void]$Names.Add($Folder + '/' + $File.Name) }
    }
}
$Records = @()
foreach ($Name in ($Names | Sort-Object)) {
    $Record = Set-UpdateProductSignature (Join-Path $Root $Name) $Name $CacheRoot
    $Records += $Record
    Write-Output "Signature $($Record.action): $Name"
}
Write-UpdateJson (Join-Path $Root 'signing-report.json') ([ordered]@{schemaVersion=1;files=$Records})
