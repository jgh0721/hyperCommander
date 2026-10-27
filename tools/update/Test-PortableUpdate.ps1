param(
    [Parameter(Mandatory=$true)][string]$BaselinePackage,
    [Parameter(Mandatory=$true)][string]$CandidatePackage,
    [string]$BuildRoot,
    [string]$TestDirectory
)
$ErrorActionPreference='Stop'
$Repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if(-not $BuildRoot){$BuildRoot=Join-Path $Repo '_build/vc2026-qt612/Release'}
$BuildRoot=[IO.Path]::GetFullPath($BuildRoot);$BaselinePackage=[IO.Path]::GetFullPath($BaselinePackage);$CandidatePackage=[IO.Path]::GetFullPath($CandidatePackage)
$Probe=Join-Path $BuildRoot 'hc_update_tests.exe';$Service=Join-Path $BuildRoot 'hc_update_service_tests.exe'
$Base=if($TestDirectory){[IO.Path]::GetFullPath($TestDirectory)}else{Join-Path $Repo '_build/portable-update-tests'}
$Base=[IO.Path]::GetFullPath($Base).TrimEnd('\','/')
$WorkspaceTests=Join-Path $Repo '_build';$TemporaryTests=Join-Path ([IO.Path]::GetTempPath()) 'hc-update-tests'
if(-not ($Base.StartsWith($WorkspaceTests+'\',[StringComparison]::OrdinalIgnoreCase) -or $Base -eq $TemporaryTests -or $Base.StartsWith($TemporaryTests+'\',[StringComparison]::OrdinalIgnoreCase))){throw 'TestDirectory must be under repository _build or TEMP/hc-update-tests.'}
$Run=Join-Path $Base ('update fixtures '+[char]0xd55c+[char]0xae00+'/'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($Run)|Out-Null
Import-Module (Join-Path $PSScriptRoot 'ProductSigning.psm1') -Force -DisableNameChecking
$null=Assert-UpdateSignature (Join-Path $BuildRoot 'HyperCommanderUpdater.exe')
Add-Type -AssemblyName System.IO.Compression.FileSystem
$Old=Get-Content -LiteralPath (Join-Path $BaselinePackage 'update-manifest.json') -Raw -Encoding UTF8|ConvertFrom-Json
$New=Get-Content -LiteralPath (Join-Path $CandidatePackage 'update-manifest.json') -Raw -Encoding UTF8|ConvertFrom-Json
$Evidence=[Collections.Generic.List[object]]::new()
function Proof([string]$Name,[object]$Details){$Evidence.Add([ordered]@{name=$Name;passed=$true;details=$Details});Write-UpdateJson (Join-Path $Run 'progress.json') @{schema=1;baseline=$Old.version;candidate=$New.version;run=$Run;results=$Evidence};Write-Output "PASS $Name"}
function Invoke-Probe([string[]]$Arguments,[switch]$Reject){
    $Command=($Arguments|ForEach-Object{'"'+$_+'"'}) -join ' '
    $Process=Start-Child $Probe $Command;$Out=$Process.StandardOutput.ReadToEndAsync();$Err=$Process.StandardError.ReadToEndAsync()
    if(-not $Process.WaitForExit(120000)){$Process.Kill();$Process.WaitForExit();throw 'Isolated update probe timed out.'}
    $Code=$Process.ExitCode;$Output=$Out.GetAwaiter().GetResult()+$Err.GetAwaiter().GetResult();$Process.Dispose()
    if($Reject){if($Code -eq 0){throw "Expected rejection: $Arguments"}}elseif($Code -ne 0){throw "Probe failed: $Arguments : $Output"}
    return ($Output -join "`n")
}
function New-Installation([string]$Name){
    $Directory=Join-Path $Run $Name;[IO.Directory]::CreateDirectory($Directory)|Out-Null
    $Zip=Join-Path $BaselinePackage ([IO.Path]::GetFileName(([Uri]$Old.asset.url).LocalPath))
    [IO.Compression.ZipFile]::ExtractToDirectory($Zip,$Directory)
    return Join-Path $Directory $Old.asset.rootDir
}
function New-Transaction([string]$Root){
    $Tx=Join-Path $Root ('.update/'+[Guid]::NewGuid().ToString());$Stage=Join-Path $Tx 'staging'
    [IO.Directory]::CreateDirectory($Stage)|Out-Null
    Copy-Item -LiteralPath (Join-Path $CandidatePackage 'update-manifest.json'),(Join-Path $CandidatePackage 'update-manifest.cat') -Destination $Tx
    $Zip=Join-Path $CandidatePackage ([IO.Path]::GetFileName(([Uri]$New.asset.url).LocalPath))
    $null=Invoke-Probe @('stage',$CandidatePackage,$Zip,$Stage,$BuildRoot)
    return $Tx
}
function Original-Hashes([string]$Root){
    $Inventory=Get-Content -LiteralPath (Join-Path $Root 'product-inventory.json') -Raw -Encoding UTF8|ConvertFrom-Json
    $Result=@{};foreach($File in $Inventory.files){$Result[$File.path]=(Get-FileHash -LiteralPath (Join-Path $Root $File.path) -Algorithm SHA256).Hash}
    $Result['product-inventory.json']=(Get-FileHash -LiteralPath (Join-Path $Root 'product-inventory.json') -Algorithm SHA256).Hash
    return $Result
}
function Assert-Original([string]$Root,$Hashes){foreach($Name in $Hashes.Keys){if((Get-FileHash -LiteralPath (Join-Path $Root $Name) -Algorithm SHA256).Hash -ne $Hashes[$Name]){throw "Original file changed: $Name"}}}
function Start-Child([string]$Image,[string]$Arguments){
    $Info=[Diagnostics.ProcessStartInfo]::new();$Info.FileName=$Image;$Info.Arguments=$Arguments;$Info.UseShellExecute=$false;$Info.CreateNoWindow=$true
    $Info.RedirectStandardOutput=$true;$Info.RedirectStandardError=$true
    $Process=[Diagnostics.Process]::new();$Process.StartInfo=$Info;[void]$Process.Start();return $Process
}
# A real product process supplies the parent identity. Only this isolated test
# instance is closed; the updater itself never terminates a product process.
$null=Invoke-Probe @('process-policy',$Run)
Proof 'restart-process-policy' 'Own background host allowed before restart; strict apply, visible/embedded viewers and other products still block'
$Root=New-Installation 'native';$Tx=New-Transaction $Root
$Config=Join-Path $Run 'parent-config';[IO.Directory]::CreateDirectory($Config)|Out-Null
$Parent=Start-Process -FilePath (Join-Path $Root 'HyperCommander.exe') -ArgumentList ('--config-dir "'+$Config+'"') -WindowStyle Hidden -PassThru
Start-Sleep -Milliseconds 1500
if($Parent.HasExited){throw 'Isolated baseline product did not start.'}
$ParentPid=$Parent.Id;$Created=$Parent.StartTime.ToUniversalTime().ToFileTimeUtc()
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class HcUpdateTestWindows {
 public delegate bool Callback(IntPtr hwnd, IntPtr state);
 [DllImport("user32.dll")] public static extern bool EnumWindows(Callback cb, IntPtr state);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int mode);
 public static void Close(uint pid) { EnumWindows((h,s)=>{uint p;GetWindowThreadProcessId(h,out p);var b=new StringBuilder(1024);GetWindowText(h,b,1024);if(p==pid&&b.ToString().Contains("HyperCommander"))PostMessage(h,0x10,IntPtr.Zero,IntPtr.Zero);return true;},IntPtr.Zero); }
}
'@
$Failure=Invoke-Probe @('apply',$Root,$Tx,"$PID",'1') -Reject
if($Failure -notmatch 'update-close-product-processes'){throw "Reused PID must still protect a live product: $Failure"}
Proof 'reused-parent-pid-live-product' $Failure
[HcUpdateTestWindows]::Close($ParentPid)
if(-not $Parent.WaitForExit(15000)){throw 'The isolated product declined normal close.'}
# Retain the exited process handle to exercise the signaled-process path.
$Sentinel=Join-Path $Root 'config/user-settings.json';[IO.Directory]::CreateDirectory((Split-Path $Sentinel -Parent))|Out-Null
[IO.File]::WriteAllText($Sentinel,'{"user":"preserve-me"}',[Text.UTF8Encoding]::new($false))
$Plugin=Join-Path $Root 'Addons/WCX/UserPlugin/custom.dat';[IO.Directory]::CreateDirectory((Split-Path $Plugin -Parent))|Out-Null
[IO.File]::WriteAllText($Plugin,'user-plugin-bytes',[Text.UTF8Encoding]::new($false))
$SentinelHash=(Get-FileHash -LiteralPath $Sentinel -Algorithm SHA256).Hash;$PluginHash=(Get-FileHash -LiteralPath $Plugin -Algorithm SHA256).Hash
$Hashes=Original-Hashes $Root
$null=Invoke-Probe @('contract');Proof 'contract' 'JSON, Windows path aliases, version limits, product ownership'
$Arguments=@('apply',$Root,$Tx,"$ParentPid","$Created")
$Staged=Join-Path $Tx 'staging/Libs/platforms/qwindows.dll'
$Stream=[IO.File]::Open($Staged,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
try{$Stream.Position=100;$OriginalByte=$Stream.ReadByte();$Stream.Position=100;$Stream.WriteByte($OriginalByte -bxor 1);$Stream.Flush()}finally{$Stream.Dispose()}
$Failure=Invoke-Probe $Arguments -Reject
if($Failure -notmatch 'update-file-hash'){throw "Unexpected staging rejection: $Failure"}
$Stream=[IO.File]::OpenWrite($Staged);try{$Stream.Position=100;$Stream.WriteByte($OriginalByte);$Stream.Flush()}finally{$Stream.Dispose()}
Assert-Original $Root $Hashes;Proof 'staged-pe-tamper' $Failure
$ManifestPath=Join-Path $Tx 'update-manifest.json';[IO.File]::AppendAllText($ManifestPath,' ',[Text.UTF8Encoding]::new($false))
$Failure=Invoke-Probe $Arguments -Reject
if($Failure -notmatch 'update-catalog'){throw "Unexpected manifest rejection: $Failure"}
Copy-Item -LiteralPath (Join-Path $CandidatePackage 'update-manifest.json') -Destination $ManifestPath -Force
Assert-Original $Root $Hashes;Proof 'signed-manifest-tamper' $Failure
$License=Join-Path $Root 'Licenses/Lua/ThirdPartyNotices.txt';$Saved=[IO.File]::ReadAllBytes($License)
[IO.File]::AppendAllText($License,'user-edit',[Text.UTF8Encoding]::new($false));$EditedHash=(Get-FileHash -LiteralPath $License -Algorithm SHA256).Hash
$Failure=Invoke-Probe $Arguments -Reject
if($Failure -notmatch 'update-file-size|update-file-hash'){throw "Exited parent was rejected before the file conflict: $Failure"}
Proof 'exited-parent-handle' $Failure
$Failure=Invoke-Probe @('apply',$Root,$Tx,"$PID",'1') -Reject
if($Failure -notmatch 'update-file-size|update-file-hash' -or (Get-FileHash -LiteralPath $License -Algorithm SHA256).Hash -ne $EditedHash){throw "Modified product file was not preserved: $Failure"}
Proof 'reused-parent-pid-exited-product' $Failure
[IO.File]::WriteAllBytes($License,$Saved);Assert-Original $Root $Hashes;Proof 'modified-owned-file-conflict' $Failure
$Collision=Join-Path $Root 'HyperCommanderUpdater.exe'
$CollisionBytes=if(Test-Path -LiteralPath $Collision){[IO.File]::ReadAllBytes($Collision)}else{$null}
$OldInventoryPath=Join-Path $Root 'product-inventory.json';$OldInventoryBytes=[IO.File]::ReadAllBytes($OldInventoryPath)
$OldInventory=Get-Content -LiteralPath $OldInventoryPath -Raw -Encoding UTF8|ConvertFrom-Json
$OldInventory.files=@($OldInventory.files|Where-Object {$_.path -ne 'HyperCommanderUpdater.exe'})
Write-UpdateJson $OldInventoryPath $OldInventory
[IO.File]::WriteAllText($Collision,'user-file-conflict')
$Failure=Invoke-Probe $Arguments -Reject
if($Failure -notmatch 'update-user-file-conflict'){throw "Unexpected conflict rejection: $Failure"}
if([IO.File]::ReadAllText($Collision) -ne 'user-file-conflict'){throw 'User collision was overwritten.'}
if($null -ne $CollisionBytes){[IO.File]::WriteAllBytes($Collision,$CollisionBytes)}else{Remove-Item -LiteralPath $Collision}
[IO.File]::WriteAllBytes($OldInventoryPath,$OldInventoryBytes)
Assert-Original $Root $Hashes;Proof 'new-path-user-conflict' $Failure
$Held=[IO.File]::Open((Join-Path $Root 'Libs/platforms/qwindows.dll'),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
try{$Failure=Invoke-Probe $Arguments -Reject}finally{$Held.Dispose()}
if($Failure -notmatch 'update-move-failed'){throw "Unexpected locked-file rejection: $Failure"}
Assert-Original $Root $Hashes
if((Get-Content -LiteralPath (Join-Path $Tx 'journal.json') -Raw|ConvertFrom-Json).state -ne 'rolledBack'){throw 'Failed replacement was not rolled back.'}
Proof 'locked-file-automatic-rollback' $Failure
$Command=($Arguments|ForEach-Object{ '"'+$_+'"' }) -join ' '
$Child=Start-Child $Probe $Command
$Deadline=[DateTime]::UtcNow.AddSeconds(30);$Interrupted=$false
while([DateTime]::UtcNow -lt $Deadline -and -not $Child.HasExited){
    if(@(Get-ChildItem -LiteralPath (Join-Path $Tx 'backup') -File -ErrorAction SilentlyContinue).Count -ge 3){$Child.Kill();$Child.WaitForExit();$Interrupted=$true;break}
    Start-Sleep -Milliseconds 5
}
if(-not $Interrupted){throw 'Could not interrupt the isolated test during replacement.'};$Child.Dispose()
$null=Invoke-Probe @('gate',$Root) -Reject
$null=Invoke-Probe @('recover',$Root);Assert-Original $Root $Hashes;Proof 'process-interruption-recovery' 'Blocked startup; reverse restoration matched all baseline hashes'
# A scanner briefly holding a product file must not roll the update back. Release the
# lock only after the move of that file has started failing.
$Transient=[IO.File]::Open((Join-Path $Root 'Libs/platforms/qwindows.dll'),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
try{
    $Child=Start-Child $Probe $Command;$Journal=Join-Path $Tx 'journal.json';$Index=-1;$Deadline=[DateTime]::UtcNow.AddSeconds(60)
    while([DateTime]::UtcNow -lt $Deadline -and -not $Child.HasExited){
        if($Index -lt 0 -and (Test-Path -LiteralPath $Journal)){try{$Files=@((Get-Content -LiteralPath $Journal -Raw|ConvertFrom-Json).files);for($I=0;$I -lt $Files.Count;++$I){if($Files[$I].path -eq 'Libs/platforms/qwindows.dll'){$Index=$I}}}catch{}}
        if($Index -gt 0 -and (Test-Path -LiteralPath (Join-Path $Tx "backup/$($Index-1)"))){Start-Sleep -Milliseconds 700;break}
        Start-Sleep -Milliseconds 5
    }
    if($Index -le 0){throw 'Transient lock test did not reach the locked file.'}
}finally{$Transient.Dispose()}
$Out=$Child.StandardOutput.ReadToEndAsync();$Err=$Child.StandardError.ReadToEndAsync()
if(-not $Child.WaitForExit(120000)){$Child.Kill();throw 'Transient lock apply timed out.'}
if($Child.ExitCode -ne 0){throw "Transient file lock rolled the update back: $($Out.GetAwaiter().GetResult())$($Err.GetAwaiter().GetResult())"};$Child.Dispose()
Proof 'transient-file-lock-retry' 'Move retried until a briefly held product file was released'
$null=Invoke-Probe @('verify',$CandidatePackage,$Root);$null=Invoke-Probe @('gate',$Root)
$null=Invoke-Probe @('recover',$Root);Assert-Original $Root $Hashes;Proof 'installed-before-health-recovery' 'Candidate verified; explicit recovery restored baseline'
$null=Invoke-Probe $Arguments
$Retired=Join-Path $Root ('.update/'+[Guid]::NewGuid().ToString());$Protected=Join-Path $Root ('.update/'+[Guid]::NewGuid().ToString())
$Completed=Get-Content -LiteralPath (Join-Path $Tx 'journal.json') -Raw -Encoding UTF8|ConvertFrom-Json;$Completed.state='completed'
foreach($Older in @($Retired,$Protected)){
    [IO.Directory]::CreateDirectory((Join-Path $Older 'backup'))|Out-Null
    Copy-Item -LiteralPath (Join-Path $Tx 'update-manifest.json'),(Join-Path $Tx 'update-manifest.cat') -Destination $Older
    Write-UpdateJson (Join-Path $Older 'journal.json') $Completed
    Copy-Item -LiteralPath (Join-Path $Tx 'backup/0') -Destination (Join-Path $Older 'backup/0')
}
$ProtectedFile=Join-Path $Protected 'do-not-delete.txt';[IO.File]::WriteAllText($ProtectedFile,'user-extra-in-completed-transaction')
$null=Invoke-Probe @('ack',$Root);$null=Invoke-Probe @('verify',$CandidatePackage,$Root)
if((Test-Path -LiteralPath $Retired) -or -not (Test-Path -LiteralPath $ProtectedFile) -or -not (Test-Path -LiteralPath (Join-Path $Protected 'backup/0'))){throw 'Completed transaction cleanup did not preserve unknown contents.'}
Proof 'completed-transaction-retention' 'Retired known completed files; preserved an entire completed transaction containing a user extra'
if(Test-Path -LiteralPath (Join-Path $Root '.update/active.json')){throw 'Health acknowledgement did not release the active transaction.'}
if(Test-Path -LiteralPath (Join-Path $Tx 'staging')){throw 'Verified staging was not reclaimed after health acknowledgement.'}
if((Get-FileHash -LiteralPath $Sentinel -Algorithm SHA256).Hash -ne $SentinelHash -or (Get-FileHash -LiteralPath $Plugin -Algorithm SHA256).Hash -ne $PluginHash){throw 'User settings/plugins changed.'}
Proof 'baseline-to-candidate-and-health' 'All candidate hashes/signatures match; user settings and plugin preserved; original-file backup retained'
$Parent.Dispose()
if(-not (Test-Path -LiteralPath (Join-Path $Root 'HyperCommanderUpdater.exe')) -or (Test-Path -LiteralPath (Join-Path $Root 'hc-updater.exe'))){throw 'Updater executable name migration failed.'}
$ServiceRoot=New-Installation 'service'
Copy-Item -LiteralPath (Join-Path $BuildRoot 'HyperCommanderUpdater.exe') -Destination (Join-Path $ServiceRoot 'HyperCommanderUpdater.exe')
$Inventory=Get-Content -LiteralPath (Join-Path $ServiceRoot 'product-inventory.json') -Raw -Encoding UTF8|ConvertFrom-Json
$Inventory.files=@($Inventory.files|Where-Object {$_.path -ne 'HyperCommanderUpdater.exe'})+@([pscustomobject]@{path='HyperCommanderUpdater.exe';sha256=(Get-FileHash -LiteralPath (Join-Path $ServiceRoot 'HyperCommanderUpdater.exe') -Algorithm SHA256).Hash.ToLowerInvariant();size=(Get-Item -LiteralPath (Join-Path $ServiceRoot 'HyperCommanderUpdater.exe')).Length})
Write-UpdateJson (Join-Path $ServiceRoot 'product-inventory.json') $Inventory
$ServiceHashes=Original-Hashes $ServiceRoot
$PreviousPlatform=$env:QT_QPA_PLATFORM;$env:QT_QPA_PLATFORM='offscreen'
try{
    # resume runs before cancel, which leaves a partial download behind.
    foreach($Mode in @('success','resume','restart','restore','redirect','http-error','cancel','blocked','launch-failure','health')){
        $ServiceConfig=Join-Path $Run ('service-config-'+$Mode);[IO.Directory]::CreateDirectory($ServiceConfig)|Out-Null
        $Command=(@($ServiceRoot,$CandidatePackage,$Mode,'--config-dir',$ServiceConfig)|ForEach-Object{'"'+$_+'"'}) -join ' '
        $Process=Start-Child $Service $Command;$Out=$Process.StandardOutput.ReadToEndAsync();$Err=$Process.StandardError.ReadToEndAsync()
        if(-not $Process.WaitForExit(120000)){$Process.Kill();$Process.WaitForExit();throw "Service $Mode timed out."}
        $Code=$Process.ExitCode;$Output=$Out.GetAwaiter().GetResult()+$Err.GetAwaiter().GetResult();$Process.Dispose()
        if($Code -ne 0){throw "Service $Mode failed (exit $Code): $Output"};Assert-Original $ServiceRoot $ServiceHashes;Proof ('service-'+$Mode) $Output
    }
}finally{$env:QT_QPA_PLATFORM=$PreviousPlatform}
Write-UpdateJson (Join-Path $Run 'evidence.json') ([ordered]@{schema=1;baseline=$Old.version;candidate=$New.version;run=$Run;results=$Evidence})
Write-Output "Update verification evidence: $Run/evidence.json"
