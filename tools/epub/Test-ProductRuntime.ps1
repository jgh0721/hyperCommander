param(
    [Parameter(Mandatory=$true)][string]$RuntimeRoot,
    [Parameter(Mandatory=$true)][string]$ExpectedSha256,
    [switch]$AsJson
)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot '../update/ProductSigning.psm1') -Force -DisableNameChecking
$RuntimeRoot=[IO.Path]::GetFullPath($RuntimeRoot).TrimEnd('\','/')
Assert-UpdatePlainPath $RuntimeRoot
$ManifestPath=Join-Path $RuntimeRoot 'epub-runtime-inventory.json'
Assert-UpdatePlainPath $ManifestPath
if($ExpectedSha256 -cnotmatch '^[0-9a-f]{64}$' -or (Get-Item -LiteralPath $ManifestPath).Length -gt 262144 -or
   (Get-FileHash -LiteralPath $ManifestPath).Hash -ine $ExpectedSha256){throw 'EPUB inventory pin mismatch'}
$Manifest=Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
if($Manifest.schemaVersion -ne 3 -or $Manifest.protocolVersion -ne 1 -or
   $Manifest.profile -cne 'epub-passive-read-only' -or $Manifest.helper -cne 'HyperEpubWeb.exe' -or
   $Manifest.readerEngine -cne 'epubjs' -or $Manifest.readerVersion -cne '0.3.93' -or
   $Manifest.readOnly -isnot [bool] -or -not $Manifest.readOnly -or
   $Manifest.diagnosticOnly -isnot [bool] -or $Manifest.diagnosticOnly -or
   $Manifest.releaseEligible -isnot [bool] -or -not $Manifest.releaseEligible -or
   $Manifest.files -isnot [array] -or $Manifest.files.Count -lt 1 -or $Manifest.files.Count -gt 511){throw 'Invalid EPUB product profile'}
$Pins=[Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
$Files=[Collections.Generic.List[object]]::new()
foreach($Entry in $Manifest.files){
    $Name=[string]$Entry.path
    if($Entry.path -isnot [string] -or $Name -cnotmatch '^[A-Za-z0-9_./-]+$' -or $Entry.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
       $Pins.ContainsKey($Name) -or $Name -ieq 'epub-runtime-inventory.json'){throw 'Invalid EPUB inventory entry'}
    $Parts=$Name.Split('/')
    if($Parts.Count -gt 32){throw 'EPUB path depth exceeded'}
    foreach($Part in $Parts){if(-not $Part -or $Part.EndsWith('.') -or $Part -match '^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)'){throw 'Unsafe EPUB path'}}
    $Path=Join-Path $RuntimeRoot $Name
    Assert-UpdatePlainPath $Path
    $Item=Get-Item -LiteralPath $Path
    if($Item.PSIsContainer -or $Item.Length -gt 536870912 -or (Get-FileHash -LiteralPath $Path).Hash -ine $Entry.sha256){throw "Changed EPUB input: $Name"}
    if($Name -match '\.(exe|dll)$'){
        if(-not (Test-UpdatePe $Path)){throw "Invalid EPUB PE: $Name"}
        $null=Assert-UpdateSignature $Path $Name
    }
    $Pins.Add($Name,$Entry.sha256)
    $Files.Add(@{path=$Name;sha256=$Entry.sha256})
}
foreach($Name in @('HyperEpubWeb.exe','QtWebEngineProcess.exe','Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll','Qt6WebEngineCore.dll',
    'Libs/platforms/qwindows.dll','bundle/bundle.json','bundle/reader.js','bundle/THIRD-PARTY-NOTICES.txt')){
    if(-not $Pins.ContainsKey($Name)){throw "Missing EPUB input: $Name"}
}
if($Manifest.bundleManifestSha256 -cnotmatch '^[0-9a-f]{64}$' -or $Pins['bundle/bundle.json'] -cne $Manifest.bundleManifestSha256){throw 'EPUB bundle pin mismatch'}
$Bundle=Get-Content -LiteralPath (Join-Path $RuntimeRoot 'bundle/bundle.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if($Bundle.schemaVersion -ne 1 -or @($Bundle.files).Count -ne 1 -or $Bundle.files[0].path -cne 'reader.js' -or
   $Bundle.files[0].sha256 -cne $Pins['bundle/reader.js']){throw 'EPUB reader hash mismatch'}
function CheckTree([string]$Directory){
    foreach($Item in Get-ChildItem -LiteralPath $Directory -Force){
        if($Item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Linked EPUB runtime entry'}
        if($Item.PSIsContainer){CheckTree $Item.FullName}
        else {
            $Name=$Item.FullName.Substring($RuntimeRoot.Length+1).Replace('\','/')
            if(-not $Pins.ContainsKey($Name) -and $Name -cnotin @('epub-runtime-inventory.json','deployment.log')){throw "Unlisted EPUB input: $Name"}
        }
    }
}
CheckTree $RuntimeRoot
$Files.Add(@{path='epub-runtime-inventory.json';sha256=$ExpectedSha256})
$Result=@{schemaVersion=1;inventorySha256=$ExpectedSha256;files=$Files.ToArray()}
if($AsJson){$Result | ConvertTo-Json -Depth 5 -Compress}else{$Result}
