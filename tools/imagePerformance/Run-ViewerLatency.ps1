param(
    [string]$Root = (Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) '_build/image-performance'),
    [string]$RemoteParent,
    [switch]$Trace
)
$ErrorActionPreference='Stop'
$Root=[IO.Path]::GetFullPath($Root)
$Repo=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Exe="$Repo/_build/vc2026-qt612/Release/hc_image_reading_tests.exe"
$Output="$Root/latency"
New-Item -ItemType Directory -Force $Output | Out-Null
# Windows widget paint boundary: async storage and filtering, excluding F3 host
# protocol, materialization, compositor and display. OS cache is not flushed.
$env:QT_QPA_PLATFORM='windows'
function Invoke-ViewerLatencyMeasurement([string]$Data,[string]$Tag){
    $env:HC_IMAGE_BENCH_ROOT=$Data
    if($Trace){$env:HC_IMAGE_TRACE="$Output/$Tag-trace.jsonl";[IO.File]::WriteAllText($env:HC_IMAGE_TRACE,'')}
    else{Remove-Item Env:HC_IMAGE_TRACE -ErrorAction SilentlyContinue}
    & $Exe optionalStorageAndFirstPaintBenchmark optionalDecodedCacheBenchmark -o "$Output/$Tag-tests.txt,txt"
    if($LASTEXITCODE){throw "Viewer latency verification failed: $Tag"}
    Copy-Item -LiteralPath "$Data/viewer-latency.json" -Destination "$Output/$Tag-latency.json" -Force
    Copy-Item -LiteralPath "$Data/viewer-cache.json" -Destination "$Output/$Tag-cache.json" -Force
}
Invoke-ViewerLatencyMeasurement $Root 'local-primary'
Invoke-ViewerLatencyMeasurement "$Root/extended" 'local-extended'
if($RemoteParent){
    $Parent=[IO.Path]::GetFullPath($RemoteParent).TrimEnd('\','/')
    if(!(Test-Path -LiteralPath $Parent -PathType Container)){throw 'Remote comparison parent is unavailable.'}
    $Scratch=Join-Path $Parent ('HyperViewerPerf-'+[Guid]::NewGuid().ToString('N'))
    if(!$Scratch.StartsWith($Parent+'\HyperViewerPerf-',[StringComparison]::OrdinalIgnoreCase)){throw 'Unexpected comparison path.'}
    $Created=[Collections.Generic.List[string]]::new()
    New-Item -ItemType Directory -Path $Scratch | Out-Null
    try{
        New-Item -ItemType Directory -Path "$Scratch/corpus" | Out-Null
        foreach($Data in @($Root,"$Root/extended")){
            foreach($Name in @('decode.jpg','decode.png','decode.webp','gray16.png','example.heic','example.avif','webtoon.png','48mp.png','24mp-avif.avif','rgb16.tiff')){
                if(Test-Path -LiteralPath "$Data/corpus/$Name"){
                    $Target=Join-Path "$Scratch/corpus" $Name
                    Copy-Item -LiteralPath "$Data/corpus/$Name" -Destination $Target
                    $Created.Add($Target)
                }
            }
        }
        Invoke-ViewerLatencyMeasurement $Scratch 'tsclient'
    }finally{
        # No recursive deletion: retain unexpected files rather than removing them.
        foreach($File in $Created){Remove-Item -LiteralPath $File -ErrorAction Continue}
        foreach($Name in @('viewer-latency.json','viewer-cache.json')){
            $File=Join-Path $Scratch $Name
            if(Test-Path -LiteralPath $File){Remove-Item -LiteralPath $File}
        }
        if(@(Get-ChildItem -LiteralPath "$Scratch/corpus" -Force).Count -eq 0){Remove-Item -LiteralPath "$Scratch/corpus"}
        if(@(Get-ChildItem -LiteralPath $Scratch -Force).Count -eq 0){Remove-Item -LiteralPath $Scratch}
    }
}
Remove-Item Env:HC_IMAGE_BENCH_ROOT,Env:HC_IMAGE_TRACE -ErrorAction SilentlyContinue
