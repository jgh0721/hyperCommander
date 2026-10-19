param([Parameter(Mandatory=$true)][string]$BaselinePackage,[Parameter(Mandatory=$true)][string]$CandidatePackage,[string]$BuildRoot,[switch]$UseBuiltHelper)
$ErrorActionPreference='Stop'
$Repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if(-not $BuildRoot){$BuildRoot=Join-Path $Repo '_build/vc2026-qt612/Release'}
$BuildRoot=[IO.Path]::GetFullPath($BuildRoot);$BaselinePackage=[IO.Path]::GetFullPath($BaselinePackage);$CandidatePackage=[IO.Path]::GetFullPath($CandidatePackage)
Import-Module (Join-Path $PSScriptRoot 'ProductSigning.psm1') -Force -DisableNameChecking
Add-Type -AssemblyName System.IO.Compression.FileSystem
$Run=Join-Path $Repo ('_build/updater-process/'+[Guid]::NewGuid().ToString('N'));[IO.Directory]::CreateDirectory($Run)|Out-Null
$Old=Get-Content -LiteralPath (Join-Path $BaselinePackage 'update-manifest.json') -Raw -Encoding UTF8|ConvertFrom-Json
$New=Get-Content -LiteralPath (Join-Path $CandidatePackage 'update-manifest.json') -Raw -Encoding UTF8|ConvertFrom-Json
[IO.Compression.ZipFile]::ExtractToDirectory((Join-Path $BaselinePackage ([IO.Path]::GetFileName(([Uri]$Old.asset.url).LocalPath))),$Run)
$Root=Join-Path $Run $Old.asset.rootDir;$Tx=Join-Path $Root ('.update/'+[Guid]::NewGuid().ToString());$Stage=Join-Path $Tx 'staging'
[IO.Directory]::CreateDirectory($Stage)|Out-Null
Copy-Item -LiteralPath (Join-Path $CandidatePackage 'update-manifest.json'),(Join-Path $CandidatePackage 'update-manifest.cat') -Destination $Tx
$Zip=Join-Path $CandidatePackage ([IO.Path]::GetFileName(([Uri]$New.asset.url).LocalPath))
& (Join-Path $BuildRoot 'hc_update_tests.exe') stage $CandidatePackage $Zip $Stage $BuildRoot
if($LASTEXITCODE -ne 0){throw 'Signed candidate staging failed.'}
$HelperSource=if($UseBuiltHelper){Join-Path $BuildRoot 'HyperCommanderUpdater.exe'}else{Join-Path $Root 'HyperCommanderUpdater.exe'}
if(-not (Test-Path -LiteralPath $HelperSource)){$HelperSource=Join-Path $Root 'hc-updater.exe'}
# Default to the actual installed helper; opt in to the fixed helper for regression.
$HelperHash=(Get-FileHash -LiteralPath $HelperSource -Algorithm SHA256).Hash
$Helper=Join-Path $Tx 'HyperCommanderUpdater.exe';Copy-Item -LiteralPath $HelperSource -Destination $Helper
$null=Assert-UpdateSignature $Helper
$Config=Join-Path $Run 'user-config';[IO.Directory]::CreateDirectory($Config)|Out-Null
Write-UpdateJson (Join-Path $Tx 'restart.json') @{configDirectory=$Config}
$Sentinel=Join-Path $Root 'user-preserved.txt';[IO.File]::WriteAllText($Sentinel,'original-user-bytes');$UserHash=(Get-FileHash -LiteralPath $Sentinel -Algorithm SHA256).Hash
Add-Type @'
using System;using System.Text;using System.Runtime.InteropServices;
public static class HcUpdaterProcessWindows {
 public delegate bool Callback(IntPtr h,IntPtr s);
 [DllImport("user32.dll")] public static extern bool EnumWindows(Callback cb,IntPtr s);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
 public static void Act(uint pid,bool close){EnumWindows((h,s)=>{uint p;GetWindowThreadProcessId(h,out p);var b=new StringBuilder(1024);GetWindowText(h,b,1024);if(p==pid&&b.ToString().Contains("HyperCommander")){if(close)PostMessage(h,16,IntPtr.Zero,IntPtr.Zero);else ShowWindow(h,0);}return true;},IntPtr.Zero);}
}
'@
$Parent=Start-Process -FilePath (Join-Path $Root 'HyperCommander.exe') -ArgumentList ('--config-dir "'+$Config+'"') -WindowStyle Hidden -PassThru
Start-Sleep -Milliseconds 1500
if($Parent.HasExited){throw 'Baseline product failed to start.'}
$Created=$Parent.StartTime.ToUniversalTime().ToFileTimeUtc()
$Arguments=@('--apply',$Root,$Tx,"$($Parent.Id)","$Created")|ForEach-Object{'"'+$_+'"'}
$Process=Start-Process -FilePath $Helper -ArgumentList ($Arguments -join ' ') -WindowStyle Hidden -PassThru
Start-Sleep -Milliseconds 1000
if($Process.HasExited -or (Test-Path -LiteralPath (Join-Path $Root '.update/active.json'))){throw 'Updater did not wait for the live parent.'}
[HcUpdaterProcessWindows]::Act($Parent.Id,$true)
if(-not $Parent.WaitForExit(15000)){throw 'Baseline product declined normal close.'};$Parent.Dispose()
if(-not $Process.WaitForExit(60000)){throw "Native updater did not finish: $Tx"}
if($Process.ExitCode -ne 0){throw "Native updater failed: $($Process.ExitCode)"};$Process.Dispose()
$Expected=Join-Path $Root 'HyperCommander.exe';$Deadline=[DateTime]::UtcNow.AddSeconds(15);$Product=$null
while([DateTime]::UtcNow -lt $Deadline -and -not $Product){$Product=Get-Process -Name HyperCommander -ErrorAction SilentlyContinue|Where-Object{$_.Path -eq $Expected}|Select-Object -First 1;Start-Sleep -Milliseconds 100}
if(-not $Product){throw 'Updated product was not relaunched.'}
$Deadline=[DateTime]::UtcNow.AddSeconds(85);$Healthy=$false
while([DateTime]::UtcNow -lt $Deadline -and -not $Product.HasExited){
    [HcUpdaterProcessWindows]::Act($Product.Id,$false)
    $Journal=Get-Content -LiteralPath (Join-Path $Tx 'journal.json') -Raw -Encoding UTF8|ConvertFrom-Json
    if($Journal.state -eq 'completed' -and -not (Test-Path -LiteralPath (Join-Path $Root '.update/active.json'))){$Healthy=$true;break}
    Start-Sleep -Milliseconds 200
}
[HcUpdaterProcessWindows]::Act($Product.Id,$true)
if(-not $Product.WaitForExit(15000)){throw 'Updated product declined normal close.'};$Product.Dispose()
if(-not $Healthy){throw 'Real main-window health acknowledgement was not received.'}
if(-not (Test-Path -LiteralPath (Join-Path $Root 'HyperCommanderUpdater.exe')) -or (Test-Path -LiteralPath (Join-Path $Root 'hc-updater.exe'))){throw 'Updater executable name migration failed.'}
& (Join-Path $BuildRoot 'hc_update_tests.exe') verify $CandidatePackage $Root
if($LASTEXITCODE -ne 0){throw 'Installed candidate verification failed.'}
if((Get-FileHash -LiteralPath $Sentinel -Algorithm SHA256).Hash -ne $UserHash){throw 'Native helper changed the user sentinel.'}
Write-UpdateJson (Join-Path $Run 'evidence.json') @{schema=1;passed=$true;baseline=$Old.version;candidate=$New.version;parentWait=$true;nativeHelper=$true;baselineHelperSha256=$HelperHash;relaunch=$true;realMainWindowHealth=$true;userPreserved=$true;root=$Root;transaction=$Tx}
Write-Output "Native updater process, relaunch and real main-window health: PASS ($Run/evidence.json)"
