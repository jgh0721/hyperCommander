param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][ValidateSet('Debug','Release')][string]$Configuration,
    [Parameter(Mandatory = $true)][string]$ReportDirectory
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Kit = 'D:\Qt\Qt-6.12.0-VC2026-X64-H-MD-2026-10-10\bin\qt-cmake.bat'
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$ReportDirectory = [IO.Path]::GetFullPath($ReportDirectory)
[IO.Directory]::CreateDirectory($ReportDirectory) | Out-Null
# Retain fresh test installations as evidence; never alter the actual user installation.
$Run = Join-Path ([IO.Path]::GetTempPath()) ('hc-document-p5-' + [Guid]::NewGuid().ToString('N'))
$Installed = Join-Path $Run 'runtime'
$Updated = Join-Path $Run 'updated'
[IO.Directory]::CreateDirectory($Installed) | Out-Null
$Checks = [Collections.Generic.List[object]]::new()
function Record([string]$Name) { $Checks.Add(@{name=$Name; passed=$true}) }
function Assert-Inventory([string]$Directory) {
    $Inventory = Get-Content -LiteralPath (Join-Path $Directory 'product-inventory.json') -Raw | ConvertFrom-Json
    if ($Inventory.schemaVersion -ne 1 -or $Inventory.configuration -ne $Configuration) { throw 'Installed inventory mismatch' }
    foreach ($File in $Inventory.files) {
        if ((Get-FileHash -LiteralPath (Join-Path $Directory $File.path)).Hash -ne $File.sha256) { throw "Installed input changed: $($File.path)" }
        if ($File.path -match '(^|/)Qt6Test') { throw 'Qt Test is included in the product inventory' }
    }
    # The viewer manifest sits in Addons/WLX/HyperViewer; its binary, helper and dependencies are ROOT-relative.
    $Manifest = Get-Content -LiteralPath (Join-Path $Directory 'Addons/WLX/HyperViewer/hyperviewer.json') -Raw | ConvertFrom-Json
    $Pinned = @{$Manifest.binary=$Manifest.sha256}
    foreach ($Section in @('helpers', 'dependencies')) {
        $Values = $Manifest.$Section
        if ($null -eq $Values) { continue }
        foreach ($Property in $Values.PSObject.Properties) { $Pinned[$Property.Name] = $Property.Value }
    }
    foreach ($Name in $Pinned.Keys) {
        if ((Get-FileHash -LiteralPath (Join-Path $Directory $Name)).Hash -ne $Pinned[$Name]) { throw "Package hash mismatch: $Name" }
    }
    foreach ($License in @('Scintilla/License.txt','Lexilla/License.txt','PDFium/LICENSE','waterColorStyle/LICENSE','Lua/ThirdPartyNotices.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Directory ('Licenses/' + $License)) -PathType Leaf)) { throw "License missing: $License" }
    }
    if (Test-Path -LiteralPath (Join-Path $Directory 'qt.conf')) { throw 'A qt.conf file must not be installed' }
}
$Sentinels = @{'HyperCommander.json'='{"P5":"preserve profile"}'; 'config/settings.json'='{"P5":"preserve settings"}';'plugins/custom/user-plugin.txt'='user plugin'; 'Addons/WLX/HyperViewer/user-options.txt'='user options'}
function Seed([string]$Directory) {
    foreach ($Relative in $Sentinels.Keys) {
        $Path = Join-Path $Directory $Relative
        [IO.Directory]::CreateDirectory((Split-Path $Path -Parent)) | Out-Null
        [IO.File]::WriteAllText($Path, $Sentinels[$Relative], [Text.UTF8Encoding]::new($false))
    }
}
function Assert-Sentinels([string]$Directory) {
    foreach ($Relative in $Sentinels.Keys) {
        if ([IO.File]::ReadAllText((Join-Path $Directory $Relative)) -cne $Sentinels[$Relative]) { throw "User file changed: $Relative" }
    }
}
function Invoke-Runtime([string]$Directory, [string]$Name, [switch]$ExpectFailure) {
    $Start = New-Object Diagnostics.ProcessStartInfo
    $Start.FileName = Join-Path $Directory 'HyperEditor.exe'
    $Start.Arguments = '--verify-runtime'
    $Start.WorkingDirectory = $Directory
    $Start.UseShellExecute = $false; $Start.CreateNoWindow = $true
    $Start.RedirectStandardOutput = $true; $Start.RedirectStandardError = $true
    $Start.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    $Start.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    foreach ($Key in @($Start.EnvironmentVariables.Keys)) { if ($Key -match '^(QT|QML)') { $Start.EnvironmentVariables.Remove($Key) } }
    $Start.EnvironmentVariables['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
    $Start.EnvironmentVariables['QT_QPA_PLATFORM'] = 'windows'
    $Process = New-Object Diagnostics.Process; $Process.StartInfo = $Start
    try {
        if (-not $Process.Start()) { throw "Runtime probe did not start: $Name" }
        $Out = $Process.StandardOutput.ReadToEndAsync(); $Err = $Process.StandardError.ReadToEndAsync()
        if (-not $Process.WaitForExit(30000)) { $Process.Kill(); $Process.WaitForExit(); throw "Runtime probe timed out: $Name" }
        $Output = $Out.GetAwaiter().GetResult(); $ErrorOutput = $Err.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $ReportDirectory ($Name + '.json')), $Output, [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText((Join-Path $ReportDirectory ($Name + '.stderr.txt')), $ErrorOutput, [Text.UTF8Encoding]::new($false))
        if ($ExpectFailure) {
            $Failure = $Output | ConvertFrom-Json
            if ($Process.ExitCode -ne 10 -or $Failure.ok -or @($Failure.errors) -notcontains 'codec-missing:jpeg') { throw "Missing codec did not produce the expected diagnostic: $Name" }
            return
        }
        if ($Process.ExitCode -ne 0) { throw "Runtime probe exited $($Process.ExitCode): $Name $ErrorOutput" }
        $Report = $Output | ConvertFrom-Json
        if (-not $Report.ok -or @($Report.saves).Count -ne 3 -or -not $Report.koreanFontAvailable) { throw "Runtime verification failed: $Name" }
        foreach ($Path in $Report.libraryPaths) {
            $Library = [IO.Path]::GetFullPath($Path).TrimEnd('\')
            $Runtime = [IO.Path]::GetFullPath($Directory).TrimEnd('\')
            if ($Library -ine $Runtime -and -not $Library.StartsWith($Runtime + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Runtime borrowed Qt plugins: $Path" }
        }
    } finally { $Process.Dispose() }
}
try {
    & $Kit --install $BuildDirectory --config $Configuration --component Runtime --prefix $Installed *> (Join-Path $ReportDirectory 'cmake-install.log')
    if ($LASTEXITCODE -ne 0) { throw 'Clean CMake installation failed' }
    Assert-Inventory $Installed; Record 'cmake-install-hashes-and-package-licenses'
    Seed $Installed
    & $Kit --install $BuildDirectory --config $Configuration --component Runtime --prefix $Installed *> (Join-Path $ReportDirectory 'cmake-reinstall.log')
    if ($LASTEXITCODE -ne 0) { throw 'CMake reinstallation failed' }
    Assert-Sentinels $Installed; Assert-Inventory $Installed; Record 'cmake-preserves-user-settings-and-plugins'
    # HyperViewer's helper is the ROOT HyperEditor.exe, so one probe covers both entry points.
    Invoke-Runtime $Installed 'standalone'
    Record 'clean-runtime-three-format-save-codecs-and-korean-font'
    $Input = Join-Path $Installed 'Licenses/Lua/ThirdPartyNotices.txt'; $Original = [IO.File]::ReadAllBytes($Input)
    try {
        # Use the same generated install script with its source relocated to our copy.
        $InstallScript = [IO.File]::ReadAllText((Join-Path $BuildDirectory 'InstallProductInventory.cmake'))
        $InstallScript = [regex]::Replace($InstallScript, '^set\(SOURCE[^\r\n]+', ('set(SOURCE "' + $Installed.Replace('\','/') + '")'))
        $ScriptPath = Join-Path $Run 'InstallTampered.cmake'
        [IO.File]::WriteAllText($ScriptPath, $InstallScript, [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText($Input, 'tampered', [Text.UTF8Encoding]::new($false))
        $RejectedDirectory = Join-Path $Run 'rejected'; Seed $RejectedDirectory
        $ErrorActionPreference = 'Continue'
        try { & $Kit "-DCMAKE_INSTALL_CONFIG_NAME=$Configuration" "-DCMAKE_INSTALL_PREFIX=$RejectedDirectory" -P $ScriptPath *> (Join-Path $ReportDirectory 'cmake-reject-tampered.log') } finally { $ErrorActionPreference = 'Stop' }
        if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath (Join-Path $RejectedDirectory 'HyperEditor.exe'))) { throw 'CMake wrote products from tampered inputs' }
        Assert-Sentinels $RejectedDirectory; Record 'cmake-rejects-tampering-before-copy'
    } finally { [IO.File]::WriteAllBytes($Input, $Original) }
    if ($Configuration -eq 'Release') {
        Seed $Updated
        & (Join-Path $Root 'tools/Install-HyperCommander.ps1') -SkipBuild -SourceDirectory $Installed -Destination $Updated *> (Join-Path $ReportDirectory 'updater.log')
        Assert-Inventory $Updated; Assert-Sentinels $Updated; Record 'updater-hashes-and-user-file-preservation'
        foreach ($Mode in @('tampered','missing')) {
            $File = if ($Mode -eq 'tampered') { $Input } else { Join-Path $Installed 'pdfium.dll' }
            $Bytes = [IO.File]::ReadAllBytes($File)
            try {
                if ($Mode -eq 'tampered') { [IO.File]::WriteAllText($File, 'tampered', [Text.UTF8Encoding]::new($false)) } else { Remove-Item -LiteralPath $File }
                $Rejected = $false
                try { & (Join-Path $Root 'tools/Install-HyperCommander.ps1') -SkipBuild -SourceDirectory $Installed -Destination $Updated *> (Join-Path $ReportDirectory ("updater-reject-$Mode.log")) } catch { $Rejected = $_.Exception.Message -match 'Missing or changed product input' }
                if (-not $Rejected) { throw "Updater accepted $Mode input or failed for another reason" }
                Assert-Inventory $Updated; Assert-Sentinels $Updated; Record "updater-rejects-$Mode-before-copy"
            } finally { [IO.File]::WriteAllBytes($File, $Bytes) }
        }
    } else {
        $Rejected = $false
        try { & (Join-Path $Root 'tools/Install-HyperCommander.ps1') -SkipBuild -SourceDirectory $Installed -Destination $Updated *> (Join-Path $ReportDirectory 'updater-reject-debug.log') } catch { $Rejected = $_.Exception.Message -eq 'Invalid Release product inventory.' }
        if (-not $Rejected) { throw 'Release updater accepted a Debug package' }
        Record 'updater-rejects-debug-product'
    }
    $Codec = Join-Path $Installed ('Libs/imageformats/qjpeg' + $(if ($Configuration -eq 'Debug') {'d'}) + '.dll')
    Move-Item -LiteralPath $Codec -Destination ($Codec + '.p5-missing')
    try { Invoke-Runtime $Installed 'missing-codec' -ExpectFailure; Record 'clean-runtime-rejects-missing-codec' }
    finally { Move-Item -LiteralPath ($Codec + '.p5-missing') -Destination $Codec }
    # Test overlay files are explicitly excluded from the product inventory.
    Copy-Item -LiteralPath (Join-Path $BuildDirectory "$Configuration/hc_smb_tests.exe") -Destination $Installed
    Copy-Item -LiteralPath (Join-Path $BuildDirectory ($Configuration + '/Qt6Test' + $(if ($Configuration -eq 'Debug') {'d'}) + '.dll')) -Destination $Installed
    foreach ($Dpr in @('1','1.5')) {
        foreach ($Case in @('documentCommandsUi','documentProductSavesUi')) {
            & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Run-DocumentCommands.ps1') -Executable (Join-Path $Installed 'hc_smb_tests.exe') -Dpr $Dpr -Case $Case -Log (Join-Path $ReportDirectory ($Case + '-dpr' + $Dpr + '.txt'))
            if ($LASTEXITCODE -ne 0) { throw "Installed UI test failed: $Case DPR $Dpr" }
        }
    }
    Record 'installed-f3-f4-and-provider-three-format-save-at-100-and-150-percent'
    Assert-Sentinels $Installed; Record 'runtime-probes-preserve-user-files'
    $Result = @{ok=$true; configuration=$Configuration; runtime=$Installed; checks=$Checks.ToArray()}
} catch {
    $Result = @{ok=$false; configuration=$Configuration; runtime=$Installed; checks=$Checks.ToArray(); error=$_.Exception.Message}
    throw
} finally {
    [IO.File]::WriteAllText((Join-Path $ReportDirectory 'results.json'), (($Result | ConvertTo-Json -Depth 6) -replace '\r?\n',[Environment]::NewLine), [Text.UTF8Encoding]::new($false))
}
Write-Output "P5 $($Configuration): $($Checks.Count) checks passed ($Installed)"
