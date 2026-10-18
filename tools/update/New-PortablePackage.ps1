param([Parameter(Mandatory=$true)][string]$Root,[Parameter(Mandatory=$true)][string]$OutputRoot,[Parameter(Mandatory=$true)][string]$Version,[string]$CacheRoot,[string]$SymbolsList)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ProductSigning.psm1') -Force -DisableNameChecking
Add-Type -AssemblyName System.IO.Compression
$Root=[IO.Path]::GetFullPath($Root); $OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
Assert-UpdatePlainPath $Root; Assert-UpdatePlainPath $OutputRoot
$BuildRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../_build'))
if (-not $OutputRoot.StartsWith($BuildRoot+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Package output must be under _build.' }
if ($Version -notmatch '^[0-9]+[.][0-9]+[.][0-9]+[.][0-9]+$' -or @($Version.Split('.')|Where-Object {[long]$_ -gt 65535}).Count) { throw "Invalid product version: [$Version]" }
$InventoryPath=Join-Path $Root 'product-inventory.json'
$Inventory=Get-Content -LiteralPath $InventoryPath -Raw -Encoding UTF8|ConvertFrom-Json
if ($Inventory.schemaVersion -ne 1 -or $Inventory.configuration -ne 'Release' -or $Inventory.version -ne $Version) { throw 'Portable packages require a matching Release inventory.' }
& (Join-Path $PSScriptRoot 'Test-ProductSignatures.ps1') -Root $Root
$Contract=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'update-contract.json') -Raw -Encoding UTF8|ConvertFrom-Json
[IO.Directory]::CreateDirectory($OutputRoot)|Out-Null
$RootName="HyperCommander-$Version-windows-x64-portable"
$ZipPath=Join-Path $OutputRoot ($RootName+'.zip')
$Temporary=Join-Path $OutputRoot ([Guid]::NewGuid().ToString('N')+'.partial')
$Names=[Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
$Sizes=[Collections.Generic.Dictionary[string,long]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($Entry in $Inventory.files) {
    $Name=[string]$Entry.path
    if ($Name -match '(^/|^[A-Za-z]:|[\\:]|(^|/)\.{1,2}(/|$))' -or $Name -match '[\x00-\x1f]' -or $Names.ContainsKey($Name)) { throw "Unsafe package path: $Name" }
    foreach($Part in $Name.Split('/')) {if(-not $Part -or $Part -match '[. ]$|^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)'){throw "Unsafe package component: $Name"}}
    $Path=Join-Path $Root $Name; Assert-UpdatePlainPath $Path
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $Entry.sha256) {throw "Changed package input: $Name"}
    $Length=(Get-Item -LiteralPath $Path).Length
    if($Length -gt $Contract.fileMaxBytes){throw "Oversized package input: $Name"}
    $Names.Add($Name,$Entry.sha256); $Sizes.Add($Name,$Length)
}
$Names.Add('product-inventory.json',(Get-FileHash -LiteralPath $InventoryPath -Algorithm SHA256).Hash.ToLowerInvariant())
$Sizes.Add('product-inventory.json',(Get-Item -LiteralPath $InventoryPath).Length)
$Unpacked=0L; foreach($Size in $Sizes.Values){$Unpacked+=$Size}
if($Unpacked -gt $Contract.unpackedMaxBytes -or $Names.Count -gt $Contract.entryMaxCount){throw 'Portable package exceeds the update resource limits.'}
try {
    $Stream=[IO.File]::Create($Temporary)
    try {
        $Zip=[IO.Compression.ZipArchive]::new($Stream,[IO.Compression.ZipArchiveMode]::Create,$true)
        try {
            foreach($Name in ($Names.Keys|Sort-Object)) {
                $Entry=$Zip.CreateEntry(($RootName+'/'+$Name),[IO.Compression.CompressionLevel]::Optimal)
                $Input=[IO.File]::OpenRead((Join-Path $Root $Name)); $Output=$Entry.Open()
                try {$Input.CopyTo($Output)} finally {$Output.Dispose();$Input.Dispose()}
            }
        } finally {$Zip.Dispose()}
    } finally {$Stream.Dispose()}
    if((Get-Item -LiteralPath $Temporary).Length -gt $Contract.archiveMaxBytes){throw 'Portable ZIP exceeds the update resource limit.'}
    $Stream=[IO.File]::OpenRead($Temporary)
    try {
        $Zip=[IO.Compression.ZipArchive]::new($Stream,[IO.Compression.ZipArchiveMode]::Read)
        try {
            if($Zip.Entries.Count -ne $Names.Count){throw 'ZIP entry count mismatch.'}
            foreach($Entry in $Zip.Entries){
                if(-not $Entry.FullName.StartsWith($RootName+'/',[StringComparison]::Ordinal)){throw 'ZIP root mismatch.'}
                $Name=$Entry.FullName.Substring($RootName.Length+1)
                if(-not $Names.ContainsKey($Name) -or $Entry.Length -ne $Sizes[$Name]){throw "Unexpected ZIP member: $Name"}
                $Input=$Entry.Open();$Sha=[Security.Cryptography.SHA256]::Create()
                try{$Hash=[BitConverter]::ToString($Sha.ComputeHash($Input)).Replace('-','').ToLowerInvariant()}finally{$Sha.Dispose();$Input.Dispose()}
                if($Hash -ine $Names[$Name]){throw "ZIP member hash mismatch: $Name"}
            }
        }finally{$Zip.Dispose()}
    }finally{$Stream.Dispose()}
    Move-Item -LiteralPath $Temporary -Destination $ZipPath -Force
}finally{if(Test-Path -LiteralPath $Temporary){Remove-Item -LiteralPath $Temporary}}
$AssetName=[IO.Path]::GetFileName($ZipPath)
$SymbolFiles=[Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
if(-not $SymbolsList){$SymbolsList=Join-Path (Split-Path $Root -Parent) 'hc-symbols-Release.txt'}
foreach($Path in Get-Content -LiteralPath $SymbolsList -Encoding UTF8){
    if(-not $Path.Trim()){continue}
    $FullPath=[IO.Path]::GetFullPath($Path);Assert-UpdatePlainPath $FullPath
    if(-not $FullPath.StartsWith($BuildRoot+'\',[StringComparison]::OrdinalIgnoreCase) -or -not (Test-Path -LiteralPath $FullPath -PathType Leaf)){throw 'Missing build-owned Release PDB.'}
    $SymbolFiles.Add([IO.Path]::GetFileName($FullPath),$FullPath)
}
if(-not $SymbolFiles.Count){throw 'No matching Release symbols were generated.'}
$SymbolsPath=Join-Path $OutputRoot ("HyperCommander-$Version-windows-x64-symbols.zip")
$SymbolsStream=[IO.File]::Create($SymbolsPath)
try{
    $Symbols=[IO.Compression.ZipArchive]::new($SymbolsStream,[IO.Compression.ZipArchiveMode]::Create,$true)
    try{foreach($Name in ($SymbolFiles.Keys|Sort-Object)){
        $Entry=$Symbols.CreateEntry($Name,[IO.Compression.CompressionLevel]::Optimal)
        $Input=[IO.File]::OpenRead($SymbolFiles[$Name]);$Output=$Entry.Open()
        try{$Input.CopyTo($Output)}finally{$Output.Dispose();$Input.Dispose()}
    }}finally{$Symbols.Dispose()}
}finally{$SymbolsStream.Dispose()}
$Manifest=[ordered]@{schema=1;product='HyperCommander';channel='stable';platform='windows';arch='x64';version=$Version;tag="v$Version";releasedAt=[DateTime]::UtcNow.ToString('o');updaterProtocol=1;layoutVersion=1;minimumFromVersion='2026.10.18.1';notesUrl="https://github.com/jgh0721/hyperCommander/releases/tag/v$Version";inventorySha256=$Names['product-inventory.json'];asset=[ordered]@{kind='zip';rootDir=$RootName;url="https://github.com/jgh0721/hyperCommander/releases/download/v$Version/$AssetName";size=(Get-Item -LiteralPath $ZipPath).Length;sha256=(Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash.ToLowerInvariant();unpackedSize=$Unpacked;fileCount=$Names.Count}}
$ManifestPath=Join-Path $OutputRoot 'update-manifest.json'
Write-UpdateJson $ManifestPath $Manifest
$CatInput=Join-Path $OutputRoot ('catalog-input-'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($CatInput)|Out-Null
Copy-Item -LiteralPath $ManifestPath -Destination (Join-Path $CatInput 'update-manifest.json')
$CatalogPath=Join-Path $OutputRoot 'update-manifest.cat'
$null=New-FileCatalog -Path $CatInput -CatalogFilePath $CatalogPath -CatalogVersion 2.0
if(-not $CacheRoot){$CacheRoot=Join-Path $PSScriptRoot '../../_build/signing-cache'}
$null=Set-UpdateProductSignature $CatalogPath 'update-manifest.cat' $CacheRoot
if((Test-FileCatalog -Path $CatInput -CatalogFilePath $CatalogPath).ToString() -ne 'Valid'){throw 'Signed manifest catalog membership failed.'}
Write-UpdateJson (Join-Path $OutputRoot 'package-evidence.json') ([ordered]@{schemaVersion=1;version=$Version;zip=$ZipPath;fileCount=$Names.Count;unpackedBytes=$Unpacked;zipBytes=$Manifest.asset.size;catalogSignature=(Assert-UpdateSignature $CatalogPath)})
Write-Output "Portable ZIP: $ZipPath ($($Manifest.asset.size) bytes; $($Names.Count) verified members)"
