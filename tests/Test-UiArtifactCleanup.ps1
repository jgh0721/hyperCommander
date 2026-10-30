param([Parameter(Mandatory=$true)][string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'tools/UiTestArtifacts.ps1')
$Base = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) ('artifact-test-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($Base) | Out-Null
function New-TestRun {
    $Directory = Join-Path $Base ('app-' + [guid]::NewGuid().ToString('N'))
    $Lease = Start-UiArtifactRun $Directory 'cleanup regression'
    [IO.File]::WriteAllText((Join-Path $Directory 'runtime.dll'), 'disposable runtime')
    [IO.File]::WriteAllText((Join-Path $Directory 'result.txt'), 'test result')
    return @{ Directory=$Directory; Lease=$Lease }
}
function Expire-TestRun($Run, [string]$Status) {
    $Path = Join-Path $Run.Directory '.ui-artifacts.json'
    $State = Get-Content $Path -Raw | ConvertFrom-Json
    $State.ownerPid = 2147483647; $State.status = $Status
    $State.expiresUtc = [datetime]::UtcNow.AddDays(-1).ToString('o')
    $State | ConvertTo-Json | Set-Content -LiteralPath $Path -Encoding UTF8
}
try {
    $Run = New-TestRun
    $Shared = Join-Path $Base 'shared.dll'
    [IO.File]::WriteAllText($Shared, 'shared runtime')
    [IO.File]::SetAttributes($Shared, [IO.FileAttributes]::ReadOnly)
    New-Item -ItemType HardLink -Path (Join-Path $Run.Directory 'Qt6Fixture.dll') -Target $Shared | Out-Null
    Complete-UiArtifactRun $Run.Directory $Run.Lease $true ''
    if (Test-Path $Run.Directory) { throw 'Successful run retained runtime' }
    if (-not ((Get-Item $Shared).Attributes -band [IO.FileAttributes]::ReadOnly)) { throw 'Cleanup changed shared snapshot attributes' }
    if ([IO.File]::ReadAllText($Shared) -ne 'shared runtime') { throw 'Cleanup changed shared snapshot content' }

    $Run = New-TestRun
    $Large = [IO.File]::Create((Join-Path $Run.Directory 'large.dmp')); $Large.SetLength(65MB); $Large.Dispose()
    Complete-UiArtifactRun $Run.Directory $Run.Lease $false ''
    if ((Test-Path (Join-Path $Run.Directory 'runtime.dll')) -or (Test-Path (Join-Path $Run.Directory 'large.dmp'))) { throw 'Failure retained runtime or oversized evidence' }
    if (-not (Test-Path (Join-Path $Run.Directory 'result.txt'))) { throw 'Failure evidence lost' }
    Invoke-UiArtifactCollection $Base
    if (-not (Test-Path $Run.Directory)) { throw 'Unexpired evidence removed' }
    Expire-TestRun $Run 'failed'; Invoke-UiArtifactCollection $Base
    if (Test-Path $Run.Directory) { throw 'Expired evidence retained' }

    $Run = New-TestRun
    Expire-TestRun $Run 'running'; Invoke-UiArtifactCollection $Base
    if (-not (Test-Path (Join-Path $Run.Directory 'runtime.dll'))) { throw 'Locked run removed' }
    $Run.Lease.Dispose(); Invoke-UiArtifactCollection $Base
    if (Test-Path (Join-Path $Run.Directory 'runtime.dll')) { throw 'Interrupted runtime retained' }
    if (-not (Test-Path (Join-Path $Run.Directory 'result.txt'))) { throw 'Interrupted evidence lost' }

    $Run = New-TestRun
    Complete-UiArtifactRun $Run.Directory $Run.Lease $true 'manual reproduction'
    if (-not (Test-Path (Join-Path $Run.Directory 'runtime.dll'))) { throw 'Explicit preservation ignored' }
    Expire-TestRun $Run 'success'; Invoke-UiArtifactCollection $Base
    if (Test-Path $Run.Directory) { throw 'Explicit preservation never expires' }

    $Legacy = Join-Path $Base ('app-' + [guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($Legacy) | Out-Null
    Invoke-UiArtifactCollection $Base
    if (-not (Test-Path $Legacy)) { throw 'Unmanaged directory removed' }
    $Rejected = $false
    try { Clear-UiArtifactRun $Base (Split-Path $Base -Parent) $false } catch { $Rejected = $true }
    if (-not $Rejected) { throw 'Outside-root deletion accepted' }
    $Run = New-TestRun
    $Run.Lease.Dispose()
    Invoke-UiArtifactCollection $Base
    if (-not (Test-Path (Join-Path $Run.Directory 'runtime.dll'))) { throw 'Live owner without lock was ignored' }
    $Outside = Join-Path $Base 'outside'; [IO.Directory]::CreateDirectory($Outside) | Out-Null
    [IO.File]::WriteAllText((Join-Path $Outside 'sentinel.txt'), 'preserve')
    $Link = Join-Path $Run.Directory 'junction'
    New-Item -ItemType Junction -Path $Link -Target $Outside | Out-Null
    $Rejected = $false
    try { Clear-UiArtifactRun $Base $Run.Directory $false } catch { $Rejected = $true }
    if (-not $Rejected -or -not (Test-Path (Join-Path $Outside 'sentinel.txt'))) { throw 'Junction protection failed' }
    [IO.Directory]::Delete($Link)
    # Exercise the runner's outer finally when runtime staging fails before launch.
    $RunnerRoot = Join-Path $Base 'runner-fixture'
    $RunnerTools = Join-Path $RunnerRoot 'tools'
    [IO.Directory]::CreateDirectory($RunnerTools) | Out-Null
    $SourceTools = Join-Path (Split-Path $PSScriptRoot -Parent) 'tools'
    foreach ($Name in @('Invoke-LocalUiTests.ps1','UiTestArtifacts.ps1')) {
        Copy-Item -LiteralPath (Join-Path $SourceTools $Name) -Destination $RunnerTools
    }
    $Shell = Join-Path $PSHOME 'powershell.exe'
    $Missing = Join-Path $RunnerRoot 'missing/runtime.exe'
    $Psi = New-Object Diagnostics.ProcessStartInfo
    $Psi.FileName = $Shell
    $Psi.Arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $RunnerTools 'Invoke-LocalUiTests.ps1') + '" -Executable "' + $Missing + '"'
    $Psi.UseShellExecute = $false; $Psi.CreateNoWindow = $true
    $Psi.RedirectStandardOutput = $true; $Psi.RedirectStandardError = $true
    $Child = [Diagnostics.Process]::Start($Psi)
    try {
        $Output = $Child.StandardOutput.ReadToEndAsync(); $ErrorOutput = $Child.StandardError.ReadToEndAsync()
        if (-not $Child.WaitForExit(30000)) { $Child.Kill(); throw 'Runner staging failure test timed out' }
        if ($Child.ExitCode -eq 0) { throw 'Runner suppressed staging failure' }
        $Runs = @(Get-ChildItem (Join-Path $RunnerRoot '_build/ui-validation') -Directory -Filter 'app-*')
        if ($Runs.Count -ne 1 -or -not (Test-Path (Join-Path $Runs[0].FullName 'failure.txt'))) { throw 'Staging failure evidence missing' }
        $State = Get-Content (Join-Path $Runs[0].FullName '.ui-artifacts.json') -Raw | ConvertFrom-Json
        if ($State.status -ne 'failed') { throw 'Staging failure did not finalize artifacts' }
    } finally { $Child.Dispose() }
    Write-Output 'PASS: success/failure cleanup, evidence size and TTL, locks, interruption, explicit retention, unmanaged paths and hardlink safety.'
} finally {
    # This test owns the entire randomly named fixture tree.
    if ($Run -and $Run.Lease) { $Run.Lease.Dispose() }
    if (Test-Path $Base) {
        foreach ($File in Get-ChildItem -LiteralPath $Base -File -Recurse -Force) { Remove-UiArtifactFile $File.FullName }
        Remove-Item -LiteralPath $Base -Recurse -Force
    }
}
