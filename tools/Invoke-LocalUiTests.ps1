param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [ValidateRange(1.0, 3.0)][double]$ScaleFactor = 1.0,
    [ValidateSet('1', '1.5')][string]$Dpr,
    [switch]$ViewerOnly,
    [switch]$CopyRuntime,
    [string]$KeepArtifactsReason,
    [switch]$DocumentOnly,
    [switch]$JobsOnly,
    [switch]$ColorRulesOnly,
    [switch]$FileToolsOnly,
    [switch]$PermissionsOnly,
    [switch]$WlxOnly,
    [switch]$DataOnly,
    [switch]$IsoOnly,
    [switch]$ArchiveOnly,
    [switch]$SettingsOnly,
    [switch]$SelectionOnly,
    [switch]$EverythingOnly,
    [switch]$FolderSizeOnly,
    [switch]$GroupingOnly,
    [ValidatePattern('^[A-Za-z0-9_]+$')][string[]]$TestFunctions,
    [switch]$EverythingLive,
    [switch]$WorkspaceOnly,
    [switch]$AutomationOnly,
    [switch]$QtitanOnly,
    [switch]$NetworkStartupOnly,
    [ValidatePattern('^[A-Za-z]:$')][string]$MappedDrive,
    [switch]$MappedIo,
    [ValidateRange(0, 2000)][int]$ViewerCycles = 0,
    [ValidateRange(10, 7200)][int]$TimeoutSeconds = 60
)
$ErrorActionPreference = 'Stop'
if ($MappedIo -and (-not $NetworkStartupOnly -or -not $MappedDrive)) { throw 'MappedIo requires NetworkStartupOnly and MappedDrive' }
$Root = Split-Path $PSScriptRoot -Parent
$RunDir = Join-Path $Root ('_build/ui-validation/app-' + [Guid]::NewGuid().ToString('N'))
$RuntimeDir = Split-Path ([IO.Path]::GetFullPath($Executable)) -Parent
. (Join-Path $PSScriptRoot 'UiTestArtifacts.ps1')
Invoke-UiArtifactCollection (Split-Path $RunDir -Parent)
$ArtifactLease = Start-UiArtifactRun $RunDir ($Executable + '; tests=' + ($TestFunctions -join ','))
$RunSucceeded = $false
try {
$DataDir = Join-Path $RunDir 'data'
[IO.Directory]::CreateDirectory($DataDir) | Out-Null
# Share only immutable Qt DLL snapshots, never links to the mutable build output.
# All application binaries, packages, settings, fixtures and logs remain per-run copies.
$SharedQtFiles = 0
function Copy-UiRuntimeDll([IO.FileInfo]$Dll, [string]$Destination) {
    if ($CopyRuntime -or $Dll.Name -notlike 'Qt6*.dll') {
        Copy-Item -LiteralPath $Dll.FullName -Destination $Destination
        return
    }
    $Hash = (Get-FileHash -LiteralPath $Dll.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $Cache = Join-Path $Root '_build/ui-validation/qt-runtime-cache'
    [IO.Directory]::CreateDirectory($Cache) | Out-Null
    $Snapshot = Join-Path $Cache ($Hash + '.dll')
    if (-not [IO.File]::Exists($Snapshot)) {
        $Pending = Join-Path $Cache ([Guid]::NewGuid().ToString('N') + '.tmp')
        try {
            Copy-Item -LiteralPath $Dll.FullName -Destination $Pending
            if ((Get-FileHash -LiteralPath $Pending -Algorithm SHA256).Hash -ine $Hash) {
                throw "Qt runtime changed during capture: $($Dll.FullName)"
            }
            try { [IO.File]::Move($Pending, $Snapshot) }
            catch { if (-not [IO.File]::Exists($Snapshot)) { throw } }
        } finally {
            if ([IO.File]::Exists($Pending)) { Remove-Item -LiteralPath $Pending }
        }
    }
    if ((Get-FileHash -LiteralPath $Snapshot -Algorithm SHA256).Hash -ine $Hash) {
        throw "Invalid Qt runtime snapshot: $Snapshot. Use -CopyRuntime for an independent copy."
    }
    [IO.File]::SetAttributes($Snapshot, ([IO.File]::GetAttributes($Snapshot) -bor [IO.FileAttributes]::ReadOnly))
    try {
        New-Item -ItemType HardLink -Path $Destination -Target $Snapshot -ErrorAction Stop | Out-Null
        $script:SharedQtFiles++
    } catch {
        # Different volumes/filesystems may not support hardlinks.
        Copy-Item -LiteralPath $Dll.FullName -Destination $Destination
    }
}
# Test only the deployed output; machine Qt variables must not supply missing DLLs.
foreach ($Dll in Get-ChildItem -LiteralPath $RuntimeDir -Filter '*.dll' -File -Recurse) {
    $Relative = $Dll.FullName.Substring($RuntimeDir.Length).TrimStart('\', '/')
    $Destination = Join-Path $RunDir $Relative
    [IO.Directory]::CreateDirectory((Split-Path $Destination -Parent)) | Out-Null
    Copy-UiRuntimeDll $Dll $Destination
}
Write-Host "Shared verified Qt runtime DLL snapshots: $SharedQtFiles"
# Exes carry qt.conf as an embedded resource; plugins come from the copied Libs/ tree.
foreach ($Name in @('hc_wcx_fixture.wcx64', 'hc_wlx_fixture.wlx64', 'hc-plugin-host-x64.exe', 'hc-search-index.exe')) {
    Copy-Item -LiteralPath (Join-Path $RuntimeDir $Name) -Destination $RunDir
}
Copy-Item -LiteralPath (Join-Path $RuntimeDir 'HyperEditor.exe') -Destination $RunDir
foreach ($Compare in @('HyperCompare.exe','HyperCompareCli.exe')) { Copy-Item -LiteralPath (Join-Path $RuntimeDir $Compare) -Destination $RunDir }
$AdminHelper = Join-Path $RuntimeDir 'HyperCommanderAdmin.exe'
if (Test-Path -LiteralPath $AdminHelper) { Copy-Item -LiteralPath $AdminHelper -Destination $RunDir }
$TestExe = Join-Path $RunDir 'hc_smb_tests.exe'
# Built-in packages keep the installed layout (manifest folder = package root).
foreach ($Package in @('Addons/WLX/HyperViewer', 'Addons/WCX/IsoImage', 'Third-Party/SevenZip', 'Third-Party/AlzEgg')) {
    $PackageDestination = Join-Path $RunDir $Package
    [IO.Directory]::CreateDirectory($PackageDestination) | Out-Null
    foreach ($File in @(Get-ChildItem -LiteralPath (Join-Path $RuntimeDir $Package) -File -ErrorAction SilentlyContinue)) { Copy-Item -LiteralPath $File.FullName -Destination $PackageDestination }
}
Copy-Item -LiteralPath $Executable -Destination $TestExe
$StartInfo = New-Object Diagnostics.ProcessStartInfo
$StartInfo.FileName = $TestExe
$StartInfo.WorkingDirectory = $RunDir
$StartInfo.Arguments = 'archiveCommandsAndBatchUi archiveCrossContainerMovesUi viewerCommandsAndPreviewUi wcxHostSettingsPersistence wcxHostApprovalUi configuredHostNavigation wlxViewerCommandsUi -o result.xml,junitxml -o result.txt,txt'
if ($PermissionsOnly) { $StartInfo.Arguments = 'permissionsUi -o result.xml,junitxml -o result.txt,txt' }
if ($FileToolsOnly) { $StartInfo.Arguments = 'fileToolsUi -o result.xml,junitxml -o result.txt,txt' }
if ($SelectionOnly) { $StartInfo.Arguments = 'itemSelectionUi -o result.xml,junitxml -o result.txt,txt' }
if ($GroupingOnly) { $StartInfo.Arguments = 'filePanelGroupingUi filePanelGroupingInputUi filePanelGroupingReentryUi filePanelGroupingSearchUi filePanelGroupingLargeUi -o result.xml,junitxml -o result.txt,txt' }
if ($EverythingOnly) { $StartInfo.Arguments = 'everythingPanelStreamingUi todoBugQuickFindSettings todoBugQuickFindInput todoBugSearchEscapeFocus todoBugQuickFindCursor todoBugQuickFindHighlight -o result.xml,junitxml -o result.txt,txt' }
if ($FolderSizeOnly) { $StartInfo.Arguments = 'folderSizesUi -o result.xml,junitxml -o result.txt,txt' }
if ($EverythingLive) { $StartInfo.Arguments = 'todoBugQuickFindEverything -o result.xml,junitxml -o result.txt,txt' }
if ($ColorRulesOnly) { $StartInfo.Arguments = 'fileColorSettingsUi jsonSettingsUi displayAndNavigationSettingsUi -o result.xml,junitxml -o result.txt,txt' }
if ($JobsOnly) { $StartInfo.Arguments = 'backgroundJobManagerUi fileJobResultPagesUi closingWindowCancelsWaitingFileJob -o result.xml,junitxml -o result.txt,txt' }
if ($ViewerOnly) { $StartInfo.Arguments = 'viewerCommandsAndPreviewUi -o result.xml,junitxml -o result.txt,txt' }
if ($DocumentOnly) { $StartInfo.Arguments = 'documentCommandsUi -o result.xml,junitxml -o result.txt,txt' }
if ($WlxOnly) { $StartInfo.Arguments = 'wlxViewerCommandsUi -o result.xml,junitxml -o result.txt,txt' }
if ($DataOnly) { $StartInfo.Arguments = 'dataPluginsUi dopusPluginsUi -o result.xml,junitxml -o result.txt,txt' }
if ($IsoOnly) { $StartInfo.Arguments = 'isoPluginUi -o result.xml,junitxml -o result.txt,txt' }
if ($ArchiveOnly) { $StartInfo.Arguments = 'sevenZipCreateUi builtinUnEggNavigation builtinSevenZipNavigation passwordAndNestedArchivesUi archiveCommandsAndBatchUi archiveCrossContainerMovesUi -o result.xml,junitxml -o result.txt,txt' }
if ($SettingsOnly) { $StartInfo.Arguments = 'jsonSettingsUi wcxHostSettingsPersistence wcxHostApprovalUi -o result.xml,junitxml -o result.txt,txt' }
if ($QtitanOnly) { $StartInfo.Arguments = 'comboAndListPreferencesUi firstRunAppearanceUi watercolorSettingsUi watercolorGridStateUi appearanceInteractionUi listLayoutSettingsUi columnWdxUi columnLuaUi columnsLargeListingUi columnPreviewAndScopesUi columnTemplatesAndAsyncUi qtitanCacheAndSortUi qtitanFileManagerUi columnWidthLayoutAndSettings columnResizeVisibilityAndScrollbars -o result.xml,junitxml -o result.txt,txt' }
if ($AutomationOnly) { $StartInfo.Arguments = 'automationCommandsUi -o result.xml,junitxml -o result.txt,txt' }
if ($WorkspaceOnly) { $StartInfo.Arguments = 'activePanelAppearanceUi displayAndNavigationSettingsUi directoryHotListPlacementUi workspaceRestoreUi protectedParentNavigationUi workspaceWatchAndMetadataUi scopedSearchUi -o result.xml,junitxml -o result.txt,txt' }
if ($ViewerCycles) { $StartInfo.Arguments = 'viewerLifetimeSoak -o result.xml,junitxml -o result.txt,txt' }
if ($NetworkStartupOnly) {
    $StartInfo.Arguments = 'networkDriveStartup -o result.xml,junitxml -o result.txt,txt'
    if ($MappedDrive) { $StartInfo.EnvironmentVariables['HC_TEST_MAPPED_DRIVE'] = $MappedDrive }
    else { $StartInfo.EnvironmentVariables['HC_TEST_DISCONNECTED_DRIVE'] = '1' }
    if ($MappedIo) { $StartInfo.EnvironmentVariables['HC_TEST_MAPPED_IO'] = '1' }
}
if ($TestFunctions) { $StartInfo.Arguments = ($TestFunctions -join ' ') + ' -o result.xml,junitxml -o result.txt,txt' }
$StartInfo.UseShellExecute = $false
$StartInfo.CreateNoWindow = $true
$StartInfo.RedirectStandardError = $true
foreach ($Key in @($StartInfo.EnvironmentVariables.Keys)) {
    if ($Key -match '^(QT|QML)') { $StartInfo.EnvironmentVariables.Remove($Key) }
}
$StartInfo.EnvironmentVariables['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
$StartInfo.EnvironmentVariables['QT_QPA_PLATFORM'] = 'windows'
$EffectiveScale = $ScaleFactor
if ($Dpr) {
    # Probe the deployed runtime with the same clean environment before scaling.
    $TestArguments = $StartInfo.Arguments
    $StartInfo.Arguments = '--probe-dpr'
    $StartInfo.RedirectStandardOutput = $true
    $Probe = New-Object Diagnostics.Process
    $Probe.StartInfo = $StartInfo
    $ProbeStarted = $false
    try {
        $ProbeStarted = $Probe.Start()
        if (-not $ProbeStarted) { throw 'Product UI DPR probe did not start' }
        $ProbeOutput = $Probe.StandardOutput.ReadToEndAsync()
        $ProbeError = $Probe.StandardError.ReadToEndAsync()
        if (-not $Probe.WaitForExit(10000)) { throw 'Product UI DPR probe timed out' }
        if ($Probe.ExitCode -ne 0) { throw "Product UI DPR probe failed: $($ProbeError.GetAwaiter().GetResult())" }
        $Culture = [Globalization.CultureInfo]::InvariantCulture
        $NativeDpr = [double]::Parse($ProbeOutput.GetAwaiter().GetResult().Trim(), $Culture)
        if ($NativeDpr -le 0 -or $NativeDpr -gt 16) { throw 'Invalid native DPR' }
        $EffectiveScale = [double]::Parse($Dpr, $Culture) / $NativeDpr
    }
    finally {
        if ($ProbeStarted -and -not $Probe.HasExited) { $Probe.Kill(); $Probe.WaitForExit(5000) | Out-Null }
        $Probe.Dispose()
    }
    $StartInfo.Arguments = $TestArguments
    $StartInfo.RedirectStandardOutput = $false
    $StartInfo.EnvironmentVariables['QT_SCALE_FACTOR_ROUNDING_POLICY'] = 'PassThrough'
    $StartInfo.EnvironmentVariables['HC_UI_EXPECT_DPR'] = $Dpr
    Write-Host "Product UI scale: native=$NativeDpr target=$Dpr multiplier=$EffectiveScale"
}
$StartInfo.EnvironmentVariables['QT_SCALE_FACTOR'] = $EffectiveScale.ToString([Globalization.CultureInfo]::InvariantCulture)
$StartInfo.EnvironmentVariables['HC_UI_LOCAL_ROOT'] = $DataDir
$StartInfo.EnvironmentVariables['HC_VIEWER_SOAK_CYCLES'] = $ViewerCycles.ToString()
$Process = New-Object Diagnostics.Process
$Process.StartInfo = $StartInfo
$Started = $false
try {
    $Started = $Process.Start()
    if (-not $Started) { throw 'Could not start local UI tests' }
    $ErrorRead = $Process.StandardError.ReadToEndAsync()
    if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) { throw 'Local UI tests timed out' }
    Get-Content -Encoding UTF8 -LiteralPath (Join-Path $RunDir 'result.txt') -Tail 20
    if ($Process.ExitCode -ne 0) { throw "Local UI tests failed: $($Process.ExitCode)" }
}
finally {
    if ($Started -and -not $Process.HasExited) { $Process.Kill(); $Process.WaitForExit(5000) | Out-Null }
    if ($Started) { [IO.File]::WriteAllText((Join-Path $RunDir 'stderr.txt'), $ErrorRead.GetAwaiter().GetResult(), (New-Object Text.UTF8Encoding($false))) }
    $Process.Dispose()
}
$RunSucceeded = $true
} catch {
    [IO.File]::WriteAllText((Join-Path $RunDir 'failure.txt'), ($_ | Out-String))
    throw
} finally {
    Complete-UiArtifactRun $RunDir $ArtifactLease $RunSucceeded $KeepArtifactsReason
}
