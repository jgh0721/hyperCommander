param(
    [Parameter(Mandatory=$true)][string]$RuntimeRoot,
    [Parameter(Mandatory=$true)][string]$ExpectedSha256,
    [string]$DestinationRoot,
    # RuntimeRoot is a product ROOT shared with other files rather than a dedicated staged runtime.
    [switch]$SharedRoot,
    [switch]$AsJson
)
$ErrorActionPreference = 'Stop'
$RuntimeRoot = [IO.Path]::GetFullPath($RuntimeRoot).TrimEnd('\','/')
function AssertPlainPath([string]$Path) {
    $Current = [IO.Path]::GetFullPath($Path)
    while ($Current) {
        $Item = Get-Item -LiteralPath $Current -Force
        if ($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked RHWP path: $Current" }
        $Parent = [IO.Directory]::GetParent($Current)
        $Current = if ($Parent) { $Parent.FullName } else { $null }
    }
}
AssertPlainPath $RuntimeRoot
$ManifestPath = Join-Path $RuntimeRoot 'runtime-inventory.json'
AssertPlainPath $ManifestPath
if ($ExpectedSha256 -cnotmatch '^[0-9a-f]{64}$' -or (Get-Item -LiteralPath $ManifestPath).Length -gt 262144 -or
    (Get-FileHash -LiteralPath $ManifestPath -Algorithm SHA256).Hash -ine $ExpectedSha256) { throw 'RHWP inventory pin mismatch.' }
$Manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
function TestSchemaVersion($Value) {
    return (($Value -is [int] -or $Value -is [long] -or $Value -is [double] -or $Value -is [decimal]) -and $Value -eq 1)
}
if (-not (TestSchemaVersion $Manifest.schemaVersion) -or $Manifest.profile -cnotin @('native-read-only','native-editor') -or
    $Manifest.sourceCommit -cne '1a76570e833917d15817415a53c09ad61ab3203f' -or
    $Manifest.releaseEligible -isnot [bool] -or -not $Manifest.releaseEligible -or
    $Manifest.bundleStorage -cne 'qt-resource' -or $Manifest.assetManifestSha256 -cnotmatch '^[0-9a-f]{64}$' -or
    $Manifest.files -isnot [array] -or @($Manifest.files).Count -lt 1 -or @($Manifest.files).Count -gt 511) { throw 'RHWP package is not eligible for product deployment.' }
$Pins = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
$Files = [Collections.Generic.List[object]]::new()
foreach ($Entry in $Manifest.files) {
    $Name = [string]$Entry.path
    $Parts = $Name.Split('/')
    if ($Entry.path -isnot [string] -or $Entry.sha256 -isnot [string] -or $Name -cnotmatch '^[A-Za-z0-9_./-]+$' -or $Parts.Count -gt 32 -or
        $Entry.sha256 -cnotmatch '^[0-9a-f]{64}$' -or $Pins.ContainsKey($Name) -or $Name -ieq 'runtime-inventory.json') { throw "Invalid RHWP entry: $Name" }
    foreach ($Part in $Parts) {
        if (-not $Part -or $Part.EndsWith('.') -or $Part -match '^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)') { throw "Unsafe RHWP path: $Name" }
    }
    $Path = Join-Path $RuntimeRoot $Name
    AssertPlainPath $Path
    $Item = Get-Item -LiteralPath $Path
    if ($Item.PSIsContainer -or $Item.Length -gt 536870912 -or (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $Entry.sha256) { throw "Changed RHWP input: $Name" }
    $Pins.Add($Name, $Entry.sha256)
    $Files.Add(@{path=$Name; sha256=$Entry.sha256})
}
# The runtime root is the product ROOT; plugins, WebEngine data and the bundle live under Libs/.
foreach ($Name in @('HyperEditorWeb.exe','QtWebEngineProcess.exe','Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll','Qt6WebEngineCore.dll','Qt6WebEngineWidgets.dll','Qt6WebChannel.dll','Libs/platforms/qwindows.dll','Libs/resources/icudtl.dat','Libs/resources/qtwebengine_resources.pak','Libs/resources/qtwebengine_resources_100p.pak','Libs/resources/qtwebengine_resources_200p.pak','Libs/resources/v8_context_snapshot.bin','Libs/translations/qtwebengine_locales/en-US.pak','Libs/bundle/asset-manifest.json','Libs/bundle/notices/RHWP-LICENSE.txt','Libs/bundle/notices/UPSTREAM-THIRD-PARTY.txt','Libs/bundle/notices/engine-lock.json')) {
    if (-not $Pins.ContainsKey($Name)) { throw "Missing RHWP runtime input: $Name" }
}
if ($Pins['Libs/bundle/asset-manifest.json'] -cne $Manifest.assetManifestSha256) { throw 'RHWP bundle pin mismatch.' }
$Bundle = Get-Content -LiteralPath (Join-Path $RuntimeRoot 'Libs/bundle/asset-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not (TestSchemaVersion $Bundle.schemaVersion) -or $Bundle.releaseEligible -isnot [bool] -or -not $Bundle.releaseEligible -or $Bundle.sourceCommit -cne $Manifest.sourceCommit) { throw 'RHWP embedded bundle is not release eligible.' }
$Editor = $Manifest.profile -ceq 'native-editor'
if ($Bundle.profile -cne $Manifest.profile -or $Bundle.capabilities.view -isnot [bool] -or -not $Bundle.capabilities.view -or
    $Bundle.capabilities.edit -isnot [bool] -or $Bundle.capabilities.edit -ne $Editor -or
    $Bundle.capabilities.save -isnot [bool] -or $Bundle.capabilities.save -ne $Editor -or
    $Bundle.capabilities.print -isnot [bool] -or
    $Bundle.capabilities.recovery -isnot [bool] -or $Bundle.capabilities.recovery) { throw 'RHWP bundle capability mismatch.' }
if ($Editor) {
    foreach ($Name in @('editor/index.html','editor/policy.json')) {
        if (@($Bundle.files | Where-Object { $_.path -ceq $Name }).Count -ne 1) { throw "Missing RHWP editor resource: $Name" }
    }
}
$Pins.Add('runtime-inventory.json', $ExpectedSha256)
$Files.Add(@{path='runtime-inventory.json';sha256=$ExpectedSha256})
# Reject links before descending, and reject DLL/data files omitted from approval.
function CheckTree([string]$Directory) {
    foreach ($Item in Get-ChildItem -LiteralPath $Directory -Force) {
        if ($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked RHWP input: $($Item.FullName)" }
        if ($Item.PSIsContainer) { CheckTree $Item.FullName }
        else {
            $Name = $Item.FullName.Substring($RuntimeRoot.Length + 1).Replace('\','/')
            if (-not $Pins.ContainsKey($Name)) { throw "Unlisted RHWP input: $Name" }
        }
    }
}
# In a shared product ROOT only the folders owned by the runtime must hold nothing unlisted.
function CheckOwned {
    foreach ($Owned in @('Libs/resources','Libs/bundle','Libs/translations/qtwebengine_locales')) {
        $Folder = Join-Path $RuntimeRoot $Owned
        if (Test-Path -LiteralPath $Folder) { AssertPlainPath $Folder; CheckTree $Folder }
    }
}
if ($SharedRoot) { CheckOwned } else { CheckTree $RuntimeRoot }
if ($DestinationRoot) {
    $DestinationRoot = [IO.Path]::GetFullPath($DestinationRoot).TrimEnd('\','/')
    $Existing = $DestinationRoot
    while (-not (Test-Path -LiteralPath $Existing)) { $Existing = [IO.Directory]::GetParent($Existing).FullName }
    AssertPlainPath $Existing
    # The destination is always the shared product ROOT.
    $RuntimeRoot = $DestinationRoot
    CheckOwned
}
$Result = @{schemaVersion=1; inventorySha256=$ExpectedSha256; files=$Files.ToArray()}
if ($AsJson) { $Result | ConvertTo-Json -Depth 5 -Compress } else { $Result }
