param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][ValidateSet('1','1.5','2')][string]$Dpr,
    [Parameter(Mandatory = $true)][string]$Log,
    [ValidatePattern('^[A-Za-z0-9_,]*$')][string]$TestFunctions
)
$ErrorActionPreference = 'Stop'
foreach ($Name in @('QT_SCALE_FACTOR','QT_SCREEN_SCALE_FACTORS','QT_AUTO_SCREEN_SCALE_FACTOR',
    'QT_ENABLE_HIGHDPI_SCALING','QT_SCALE_FACTOR_ROUNDING_POLICY','QT_FONT_DPI')) {
    [Environment]::SetEnvironmentVariable($Name,$null,'Process')
}
$Native = (& $Executable --probe-dpr | Out-String).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Document DPR probe failed' }
$Culture = [Globalization.CultureInfo]::InvariantCulture
$NativeDpr = [double]::Parse($Native,$Culture)
if ($NativeDpr -le 0 -or $NativeDpr -gt 16) { throw "Invalid native DPR: $Native" }
$env:QT_SCALE_FACTOR = ([double]::Parse($Dpr,$Culture)/$NativeDpr).ToString('R',$Culture)
$env:QT_SCALE_FACTOR_ROUNDING_POLICY = 'PassThrough'
$env:HC_DOCUMENT_EXPECT_DPR = $Dpr
$SelectedFunctions = @()
if ($TestFunctions -and $TestFunctions -ne 'all') { $SelectedFunctions = $TestFunctions.Split(',') }
& $Executable @SelectedFunctions -o ($Log + ',txt')
$Result = $LASTEXITCODE
Get-Content -LiteralPath $Log
exit $Result
