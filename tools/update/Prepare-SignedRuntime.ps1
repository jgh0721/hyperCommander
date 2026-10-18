param([Parameter(Mandatory=$true)][string]$Source,[Parameter(Mandatory=$true)][string]$ExpectedSha256,[Parameter(Mandatory=$true)][string]$Destination,[string]$CacheRoot)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ProductSigning.psm1') -Force -DisableNameChecking
$Source = [IO.Path]::GetFullPath($Source); $Destination = [IO.Path]::GetFullPath($Destination)
if (-not $CacheRoot) { $CacheRoot = Join-Path $PSScriptRoot '../../_build/signing-cache' }
if ($Source -eq $Destination) { throw 'Original approved runtime must be preserved.' }
$Verified = & (Join-Path $PSScriptRoot '../rhwp/Test-ProductRuntime.ps1') -RuntimeRoot $Source -ExpectedSha256 $ExpectedSha256
$Inventory = Get-Content -LiteralPath (Join-Path $Source 'runtime-inventory.json') -Raw -Encoding UTF8 | ConvertFrom-Json
Assert-UpdatePlainPath $Destination
[IO.Directory]::CreateDirectory($Destination) | Out-Null
foreach ($Entry in $Verified.files) {
    $InputPath = Join-Path $Source $Entry.path; $OutputPath = Join-Path $Destination $Entry.path
    Assert-UpdatePlainPath $OutputPath
    [IO.Directory]::CreateDirectory((Split-Path $OutputPath -Parent)) | Out-Null
    Copy-Item -LiteralPath $InputPath -Destination $OutputPath -Force
    if (Test-UpdatePe $OutputPath) {
        $Record = Set-UpdateProductSignature $OutputPath $Entry.path $CacheRoot
        Write-Output "Runtime signature $($Record.action): $($Entry.path)"
    }
}
foreach ($Entry in $Inventory.files) { $Entry.sha256 = (Get-FileHash -LiteralPath (Join-Path $Destination $Entry.path) -Algorithm SHA256).Hash.ToLowerInvariant() }
Write-UpdateJson (Join-Path $Destination 'runtime-inventory.json') $Inventory
$Pin = (Get-FileHash -LiteralPath (Join-Path $Destination 'runtime-inventory.json') -Algorithm SHA256).Hash.ToLowerInvariant()
$null = & (Join-Path $PSScriptRoot '../rhwp/Test-ProductRuntime.ps1') -RuntimeRoot $Destination -ExpectedSha256 $Pin
Write-UpdateJson ($Destination + '.provenance.json') ([ordered]@{source=$Source;sourceInventorySha256=$ExpectedSha256;runtime=$Destination;inventorySha256=$Pin})
Write-Output "Signed runtime inventory: $Pin"
