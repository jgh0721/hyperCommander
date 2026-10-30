param([Parameter(Mandatory=$true)][string]$BuildDirectory)
$ErrorActionPreference='Stop'
$SourceRoot=Split-Path $PSScriptRoot -Parent
$Tokens=$null; $Errors=$null
$Ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $SourceRoot 'tools/Invoke-LocalUiTests.ps1'),[ref]$Tokens,[ref]$Errors)
if($Errors.Count){throw 'UI runner parse failed'}
$Function=$Ast.Find({param($Node) $Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -eq 'Copy-UiRuntimeDll'},$true)
if(-not $Function){throw 'Runtime snapshot staging function missing'}
. ([ScriptBlock]::Create($Function.Extent.Text))
$Root=Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) ('ui-snapshot-test-'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($Root)|Out-Null
. (Join-Path $SourceRoot 'tools/UiTestArtifacts.ps1')
try {
$Source=Join-Path $Root 'Qt6SnapshotFixture.dll'
[IO.File]::WriteAllText($Source,'first runtime')
$CopyRuntime=$false; $SharedQtFiles=0
$First=Join-Path $Root 'first.dll'; $Second=Join-Path $Root 'second.dll'
Copy-UiRuntimeDll (Get-Item $Source) $First
Copy-UiRuntimeDll (Get-Item $Source) $Second
if($SharedQtFiles -ne 2){throw 'Snapshot hardlink reuse failed'}
if((Get-ChildItem (Join-Path $Root '_build/ui-validation/qt-runtime-cache') -File).Count -ne 1){throw 'Identical content created multiple snapshots'}
$Rejected=$false
try{[IO.File]::WriteAllText($First,'tampered')}catch [UnauthorizedAccessException]{$Rejected=$true}
if(-not $Rejected){throw 'Shared runtime was writable'}
[IO.File]::WriteAllText($Source,'second runtime')
$Third=Join-Path $Root 'third.dll'; Copy-UiRuntimeDll (Get-Item $Source) $Third
if([IO.File]::ReadAllText($First) -cne 'first runtime' -or [IO.File]::ReadAllText($Third) -cne 'second runtime'){throw 'Build output change leaked into an earlier snapshot'}
$CopyRuntime=$true
$Independent=Join-Path $Root 'independent.dll'; Copy-UiRuntimeDll (Get-Item $Source) $Independent
[IO.File]::WriteAllText($Independent,'independent mutation')
if([IO.File]::ReadAllText($Third) -cne 'second runtime'){throw 'Copy fallback modified the snapshot'}
Write-Output 'Runtime snapshot reuse, write protection, source-change isolation and copy fallback passed.'
} finally {
    foreach ($File in Get-ChildItem -LiteralPath $Root -File -Recurse -Force) { Remove-UiArtifactFile $File.FullName }
    Remove-Item -LiteralPath $Root -Recurse -Force
}
