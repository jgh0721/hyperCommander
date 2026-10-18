param([Parameter(Mandatory=$true)][string]$Path,[string]$RelativePath,[string]$CacheRoot)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ProductSigning.psm1') -Force -DisableNameChecking
if (-not $RelativePath) { $RelativePath = [IO.Path]::GetFileName($Path) }
if (-not $CacheRoot) { $CacheRoot = Join-Path $PSScriptRoot '../../_build/signing-cache' }
$Result = Set-UpdateProductSignature $Path $RelativePath $CacheRoot
Write-Output "Signature $($Result.action): $RelativePath"
