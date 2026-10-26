param([Parameter(Mandatory=$true)][string]$Root)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ProductSigning.psm1') -Force -DisableNameChecking
$Root = [IO.Path]::GetFullPath($Root); Assert-UpdatePlainPath $Root
$Inventory = Get-Content -LiteralPath (Join-Path $Root 'product-inventory.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($Inventory.schemaVersion -ne 1 -or $Inventory.product -ne 'HyperCommander' -or $Inventory.configuration -ne 'Release' -or
    $Inventory.layoutVersion -ne 1 -or $Inventory.updaterProtocol -ne 1 -or $Inventory.version -notmatch '^[0-9]+[.][0-9]+[.][0-9]+[.][0-9]+$' -or
    @($Inventory.files).Count -lt 1 -or @($Inventory.files).Count -ge 4096) { throw 'Invalid signed Release product inventory.' }
$Count = 0
$Seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($Entry in $Inventory.files) {
    if ($Entry.path -isnot [string] -or $Entry.sha256 -notmatch '^[A-Fa-f0-9]{64}$' -or
        $Entry.path -match '(^/|^[A-Za-z]:|[\\:]|(^|/)\.{1,2}(/|$)|[\x00-\x1f])' -or -not $Seen.Add($Entry.path)) { throw 'Invalid signed product inventory path.' }
    foreach ($Part in $Entry.path.Split('/')) { if (-not $Part -or $Part -match '[. ]$|^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)') { throw 'Invalid signed product path component.' } }
    if ($Entry.path -cnotmatch '^(HyperCommander(?:Admin|Updater)?\.exe|HyperEditor(?:Web)?\.exe|HyperCompare(?:Cli)?\.exe|QtWebEngineProcess\.exe|hc-(?:plugin-host-x64|plugin-inspect|search-index)\.exe|Everything64\.dll|runtime-inventory\.json|(?:Qt6|vcruntime|msvcp|concrt|d3dcompiler_|opengl32sw|pdfium)[A-Za-z0-9_.]*\.dll|Libs/.+|Licenses/.+|Addons/WCX/IsoImage/.+|Addons/WLX/HyperViewer/.+|Third-Party/(?:SevenZip|AlzEgg)/.+)$') { throw "Unowned product path: $($Entry.path)" }
    $Path = Join-Path $Root $Entry.path; Assert-UpdatePlainPath $Path
    if (($Entry.size -isnot [int] -and $Entry.size -isnot [long]) -or $Entry.size -lt 0 -or $Entry.size -gt 536870912 -or
        (Get-Item -LiteralPath $Path).Length -ne $Entry.size) { throw "Invalid signed product size: $($Entry.path)" }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $Entry.sha256) { throw "Changed signed product: $($Entry.path)" }
    $IsPe = Test-UpdatePe $Path
    if (-not $IsPe -and $Entry.path -match '\.(exe|dll|wcx64|wlx64|wfx64|wdx64)$') { throw "Invalid product PE: $($Entry.path)" }
    if ($IsPe) { $null = Assert-UpdateSignature $Path $Entry.path; ++$Count }
}
foreach ($Required in @('HyperCommander.exe','HyperCommanderAdmin.exe','HyperEditor.exe','HyperCompare.exe','HyperCompareCli.exe','HyperCommanderUpdater.exe','hc-plugin-host-x64.exe','hc-plugin-inspect.exe','hc-search-index.exe',
    'Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll','Qt6Network.dll','Libs/platforms/qwindows.dll','Addons/WLX/HyperViewer/HyperViewer.dll',
    'Addons/WCX/IsoImage/hc_isoimage_wcx.wcx64','Addons/WCX/IsoImage/hc_iso_core.dll','Third-Party/SevenZip/7z.dll','pdfium.dll','Everything64.dll')) {
    if (-not $Seen.Contains($Required)) { throw "Missing required signed product: $Required" }
}
if ($Count -lt 1) { throw 'No signed product PE was verified.' }
Write-Output "Verified $Count signed product PE files and $(@($Inventory.files).Count) inventory hashes."
