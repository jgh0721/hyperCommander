# After the product Release build: Prepare-ImageComparison.ps1,
# Build-ComparisonBackends.ps1, this script, Run-ExtraComparison.ps1,
# Collect-ComparisonEnvironment.ps1. Run each sequentially on an idle machine.
param(
    [string]$Root = (Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) '_build/image-performance')
)
$ErrorActionPreference='Stop'
$Root=[IO.Path]::GetFullPath($Root)
$Repo=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Python=[IO.File]::ReadAllText("$Root/python-path.txt").Trim()
$env:PYTHONPATH="$Root/python"
$env:HC_IMAGE_DLL_DIRS="$Root/vendor/vips-dev-8.18/bin"
$env:PATH="D:/Qt/Qt-6.12.0-VC2026-X64-H-MD-2026-10-10/bin;$env:HC_IMAGE_DLL_DIRS;$env:PATH"
$env:QT_PLUGIN_PATH="$Repo/_build/vc2026-qt612/Release/Libs"
$env:QT_QPA_PLATFORM='offscreen'
$env:HC_BENCH_THREADS='4'
$env:VIPS_CONCURRENCY='4'
$env:MAGICK_THREAD_LIMIT='4'
$env:OMP_NUM_THREADS='4'
$env:OPENBLAS_NUM_THREADS='4'
$Exe="$Root/native/Release/hc_image_comparison.exe"
$Script=Join-Path $PSScriptRoot 'compare.py'
$Statuses=@()
if(!(Test-Path "$Root/matrix.json")){
    & $Python $Script $Root generate
    if($LASTEXITCODE){throw 'Corpus generation failed.'}
}
function Native([string]$Engine,[string]$Operation,[string]$Selector=''){
    & $Exe $Root $Engine $Operation $Selector *> "$Root/$Engine-$Operation.log"
    $script:Statuses+=@{engine=$Engine;operation=$Operation;exit=$LASTEXITCODE}
    Write-Output "$Engine $Operation exit=$LASTEXITCODE"
}
function Python([string]$Engine,[string]$Operation){
    $Action=if($Engine -eq 'imagecodecs' -and $Operation -eq 'decode'){'isolated-decodes'}else{'run'}
    & $Python $Script $Root $Action --engine $Engine --operation $Operation *> "$Root/python-$Engine-$Operation.log"
    $script:Statuses+=@{engine=$Engine;operation=$Operation;exit=$LASTEXITCODE}
    Write-Output "$Engine $Operation exit=$LASTEXITCODE"
}
Native 'native' 'validate'
foreach($Engine in @('reference','native','qt','wic','stb','zimg','gpu','warp')){Native $Engine 'resize'}
foreach($Engine in @('pillow','opencv','opencv-linear','vips','vips-linear','magickcore','scipy','skimage','swscale','zscale','libplacebo')){Python $Engine 'resize'}
foreach($Engine in @('qt','qt-scaled')){Native $Engine 'decode'}
foreach($Engine in @('pillow','opencv','vips','imagecodecs','pillow-heif')){Python $Engine 'decode'}
foreach($Threads in @(1,2,4,8)){
    $env:HC_IMAGE_RENDER_THREADS="$Threads"
    Native "native-t$Threads" 'resize' '-0.5-'
    $env:HC_IMAGE_CODEC_THREADS="$Threads"
    $env:HC_AVIF_COMPARE_DLL="$Root/avif-yuv-build/Release/avif.dll"
    Native "avif-exact-t$Threads" 'decode'
    Native "avif-yuv-t$Threads" 'decode'
}
Remove-Item Env:HC_IMAGE_RENDER_THREADS,Env:HC_IMAGE_CODEC_THREADS,Env:HC_AVIF_COMPARE_DLL
& $Python $Script $Root quality
[IO.File]::WriteAllText("$Root/run-status.json",($Statuses|ConvertTo-Json -Depth 5))
