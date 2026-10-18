param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][ValidateSet('1','1.5')][string]$Dpr,
    [Parameter(Mandatory = $true)][string]$Log,
    [ValidateSet("documentCommandsUi","documentProductSavesUi")][string]$Case = "documentCommandsUi"
)
$ErrorActionPreference = 'Stop'
$Executable = [IO.Path]::GetFullPath($Executable)
$Log = [IO.Path]::GetFullPath($Log)
$RunRoot = Join-Path (Split-Path $Log -Parent) ('document-commands-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($RunRoot) | Out-Null
$Start = New-Object Diagnostics.ProcessStartInfo
$Start.FileName = $Executable
$Start.WorkingDirectory = Split-Path $Executable -Parent
$Start.UseShellExecute = $false
$Start.CreateNoWindow = $true
$Start.RedirectStandardOutput = $true
$Start.RedirectStandardError = $true
foreach ($Name in @($Start.EnvironmentVariables.Keys)) {
    if ($Name -match '^(QT|QML)') { $Start.EnvironmentVariables.Remove($Name) }
}
$Start.EnvironmentVariables['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
$Start.EnvironmentVariables['QT_QPA_PLATFORM'] = 'windows'
$Start.EnvironmentVariables['HC_UI_LOCAL_ROOT'] = $RunRoot
$Start.EnvironmentVariables['HC_TEST_CONFIG_DIR'] = Join-Path $RunRoot 'config'
$Culture = [Globalization.CultureInfo]::InvariantCulture
$Start.Arguments = '--probe-dpr'
$Probe = New-Object Diagnostics.Process
$Probe.StartInfo = $Start
try {
    if (-not $Probe.Start()) { throw 'Document command DPR probe did not start' }
    $Output = $Probe.StandardOutput.ReadToEndAsync()
    $ErrorOutput = $Probe.StandardError.ReadToEndAsync()
    if (-not $Probe.WaitForExit(10000)) { $Probe.Kill(); throw 'Document command DPR probe timed out' }
    if ($Probe.ExitCode -ne 0) { throw $ErrorOutput.GetAwaiter().GetResult() }
    $Native = [double]::Parse($Output.GetAwaiter().GetResult().Trim(), $Culture)
    if ($Native -le 0 -or $Native -gt 16) { throw 'Invalid document command DPR' }
} finally { $Probe.Dispose() }
$Start.EnvironmentVariables['QT_SCALE_FACTOR'] = ([double]::Parse($Dpr,$Culture)/$Native).ToString('R',$Culture)
$Start.EnvironmentVariables['QT_SCALE_FACTOR_ROUNDING_POLICY'] = 'PassThrough'
$Start.EnvironmentVariables['HC_UI_EXPECT_DPR'] = $Dpr
$Start.Arguments = $Case + ' -o "' + $Log + ',txt"'
$Process = New-Object Diagnostics.Process
$Process.StartInfo = $Start
try {
    if (-not $Process.Start()) { throw 'Document command test did not start' }
    $Output = $Process.StandardOutput.ReadToEndAsync()
    $ErrorOutput = $Process.StandardError.ReadToEndAsync()
    $Timer = [Diagnostics.Stopwatch]::StartNew()
    while (-not $Process.WaitForExit(500)) {
        if ($Timer.Elapsed.TotalSeconds -gt 150) { $Process.Kill(); throw 'Document command test timed out' }
    }
    $Result = $Process.ExitCode
    $Output.GetAwaiter().GetResult()
    $ErrorOutput.GetAwaiter().GetResult()
    if (Test-Path -LiteralPath $Log) { Get-Content -LiteralPath $Log -Encoding UTF8 }
} finally { $Process.Dispose() }
exit $Result
