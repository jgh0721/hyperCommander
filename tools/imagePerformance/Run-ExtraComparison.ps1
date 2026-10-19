param([string]$Root = (Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) '_build/image-performance'),
    [ValidateSet('All','Resize','Decode')][string]$Phase='All')
$ErrorActionPreference='Stop'
$Root=[IO.Path]::GetFullPath($Root)
$Repo=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Python=[IO.File]::ReadAllText("$Root/python-path.txt").Trim()
$env:PYTHONPATH="$Root/extra-python;$Root/python"
$env:HC_IMAGE_DLL_DIRS="$Root/vendor/vips-dev-8.18/bin"
$env:PATH="D:/Qt/Qt-6.12.0-VC2026-X64-H-MD-2026-10-10/bin;$env:HC_IMAGE_DLL_DIRS;$env:PATH"
$env:QT_PLUGIN_PATH="$Repo/_build/vc2026-qt612/Release/Libs"
$env:QT_QPA_PLATFORM='offscreen'
$env:HC_BENCH_THREADS='4';$env:VIPS_CONCURRENCY='4';$env:MAGICK_THREAD_LIMIT='4';$env:OMP_NUM_THREADS='4';$env:OPENBLAS_NUM_THREADS='4'
$Exe="$Root/native/Release/hc_image_comparison.exe"
$Compare=Join-Path $PSScriptRoot 'compare.py'
$Extra=Join-Path $PSScriptRoot 'extra_compare.py'
$Statuses=@()
if(!(Test-Path "$Root/extended/matrix.json")){
    & $Python (Join-Path $PSScriptRoot 'extend_corpus.py') $Root
    if($LASTEXITCODE){throw 'Extended corpus generation failed.'}
}
function Native([string]$Data,[string]$Engine,[string]$Operation,[string]$Selector=''){
    $ErrorActionPreference='Continue'
    $Process=Start-Process -FilePath $Exe -ArgumentList @("`"$Data`"",$Engine,$Operation,"`"$Selector`"") -WindowStyle Hidden -PassThru -RedirectStandardOutput "$Data/$Engine-$Operation.log" -RedirectStandardError "$Data/$Engine-$Operation.stderr.log"
    $Code=124
    $null=$Process.Handle
    if($Process.WaitForExit(180000)){$Code=$Process.ExitCode}else{$Process.Kill();$Process.WaitForExit()}
    $script:Statuses+=@{cohort=$Data;engine=$Engine;operation=$Operation;exit=$Code}
    Write-Output "$Data $Engine $Operation exit=$Code"
}
function Python([string]$Data,[string]$Engine,[string]$Operation){
    $ErrorActionPreference='Continue'
    $Action=if($Engine -eq 'imagecodecs' -and $Operation -eq 'decode'){'isolated-decodes'}else{'run'}
    & $Python $Compare $Data $Action --engine $Engine --operation $Operation *> "$Data/python-$Engine-$Operation.log"
    $script:Statuses+=@{cohort=$Data;engine=$Engine;operation=$Operation;exit=$LASTEXITCODE}
    Write-Output "$Data $Engine $Operation exit=$LASTEXITCODE"
}
Native $Root 'native' 'validate'
if($Phase -ne 'Decode'){
foreach($Data in @($Root,"$Root/extended")){
    foreach($Engine in @('skia','oiio','simd')){
        $ErrorActionPreference='Continue'
        & $Python $Extra $Data $Engine --vendor-root $Root *> "$Data/$Engine-extra.log"
        $Statuses+=@{cohort=$Data;engine=$Engine;operation='resize';exit=$LASTEXITCODE}
        Write-Output "$Data $Engine resize exit=$LASTEXITCODE"
    }
}
$Data="$Root/extended"
foreach($Engine in @('reference','native','qt','wic','stb','zimg','gpu')){Native $Data $Engine 'resize'}
foreach($Engine in @('pillow','opencv','opencv-linear','vips','vips-linear','magickcore','libplacebo')){Python $Data $Engine 'resize'}
}
if($Phase -ne 'Resize'){
foreach($Data in @($Root,"$Root/extended")){
    foreach($Engine in @('qt','qt-scaled','wic','wic-scaled')){Native $Data $Engine 'decode'}
    foreach($Engine in @('pillow','opencv','vips','imagecodecs','pillow-heif','spng')){Python $Data $Engine 'decode'}
    foreach($Threads in @(1,2,4,8)){
        $env:HC_IMAGE_CODEC_THREADS="$Threads"
        $env:HC_AVIF_COMPARE_DLL="$Root/avif-yuv-build/Release/avif.dll"
        Native $Data "avif-exact-t$Threads" 'decode'
        Native $Data "avif-yuv-t$Threads" 'decode'
    }
}
}
Native $Root 'gpu-cold' 'resize' 'large-0.25-lanczos3'
Remove-Item Env:HC_IMAGE_CODEC_THREADS,Env:HC_AVIF_COMPARE_DLL -ErrorAction SilentlyContinue
foreach($Data in @($Root,"$Root/extended")){& $Python $Compare $Data quality}
& $Python (Join-Path $PSScriptRoot 'analyze.py') $Root
[IO.File]::WriteAllText("$Root/extra-$Phase-run-status.json",($Statuses|ConvertTo-Json -Depth 6))
