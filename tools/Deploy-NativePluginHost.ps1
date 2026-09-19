param(
    [Parameter(Mandatory = $true)][ValidateSet('Debug', 'Release')][string]$Configuration,
    [Parameter(Mandatory = $true)][string]$QtRoot,
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$VisualStudioRoot,
    [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$BuildDir = Join-Path $Root ('_build/msbuild-plugin-host-' + $Configuration.ToLowerInvariant())
$QtCMake = Join-Path $QtRoot 'bin/qt-cmake.bat'
# Use the same host sources and deployment rules as the primary CMake build.
# Separate per-configuration directories also permit concurrent native builds.
& $QtCMake -S $Root -B $BuildDir -G 'Visual Studio 18 2026' -A x64 -T 'v145,host=x64' `
    -DBUILD_TESTING=OFF -DHC_BUILD_SMB_TESTS=OFF
if ($LASTEXITCODE -ne 0) { throw "Plugin host configure failed: $LASTEXITCODE" }
& $QtCMake --build $BuildDir --config $Configuration --target hc_iso_package --parallel 4
if ($LASTEXITCODE -ne 0) { throw "Built-in ISO package build failed: $LASTEXITCODE" }
if ($PrepareOnly) { return }
& $QtCMake --build $BuildDir --config $Configuration --target hc-plugin-host-x64 hc-search-index --parallel 4
if ($LASTEXITCODE -ne 0) { throw "Plugin host build failed: $LASTEXITCODE" }
$Destination = Split-Path ([IO.Path]::GetFullPath($Executable)) -Parent
Copy-Item -LiteralPath (Join-Path $BuildDir "$Configuration/hc-plugin-host-x64.exe") -Destination $Destination
Copy-Item -LiteralPath (Join-Path $BuildDir "$Configuration/hc-search-index.exe") -Destination $Destination
$BuiltinDestination = Join-Path $Destination 'BuiltInPlugin/IsoImage'
[IO.Directory]::CreateDirectory($BuiltinDestination) | Out-Null
Get-ChildItem -LiteralPath (Join-Path $BuildDir "$Configuration/BuiltInPlugin/IsoImage") -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $BuiltinDestination
}
$DebugSuffix = if ($Configuration -eq 'Debug') { 'd' } else { '' }
& $QtCMake "-DHC_DEPLOY_EXE=$Executable" "-DHC_QT_BIN=$QtRoot/bin" `
    "-DHC_GUI_DLL=$QtRoot/bin/Qt6Gui$DebugSuffix.dll" "-DHC_CONFIG=$Configuration" `
    "-DHC_VS_DIR=$VisualStudioRoot" -DHC_WITH_TEST=OFF -DHC_WITH_PLUGINS=ON `
    "-DHC_PLUGIN_MANIFEST=$BuildDir/hc-qt-plugins-$Configuration.cmake" `
    "-DHC_RUNTIME_FILE=$Root/externalLibs/Everything/src/dll/Everything64.dll" `
    -P "$Root/cmake/DeployQt.cmake"
if ($LASTEXITCODE -ne 0) { throw "Native runtime deployment failed: $LASTEXITCODE" }
