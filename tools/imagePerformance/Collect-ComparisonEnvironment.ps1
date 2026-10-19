param([string]$Root = (Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) '_build/image-performance'))
$ErrorActionPreference='Stop'
$Root=[IO.Path]::GetFullPath($Root)
$Repo=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Sources=@()
foreach($Name in @('zimg.zip','stb_image_resize2.h','vips.zip','simd.zip','libyuv.tar.gz')){
    $Sources+=@{name=$Name;sha256=(Get-FileHash -LiteralPath "$Root/vendor/$Name").Hash}
}
$Inputs=@()
foreach($Data in @($Root,"$Root/extended")){
    Get-ChildItem -LiteralPath "$Data/corpus" -File | ForEach-Object {
        $Inputs+=@{cohort=(Split-Path $Data -Leaf);name=$_.Name;bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
    }
}
$Report=@{
    capturedUtc=[DateTime]::UtcNow.ToString('o');revision=(& git -C $Repo rev-parse HEAD)
    productChanges='Working tree source hashes below; revision is the pre-commit base.'
    cpu=(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors)
    gpu=(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion)
    os=(Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,BuildNumber)
    ffmpeg=(& ffmpeg -version 2>&1 | Out-String)
    toolchain='VS 2026 v145; Hybrid CRT; Qt 6.12.0 2026-10-10 kit; Release'
    sources=$Sources;inputs=$Inputs
    libraries=@{zimg='3.0.6';stb='a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d';vips='8.18.7';simd='8.0.166 AVX2 build';libyuv='644251f25 / 1924';libavif='1.4.2';dav1d='1.5.3';libheif='1.23.6';libde265='1.1.3'}
    hashes=@(Get-FileHash -LiteralPath "$Repo/UniqueLibs/document/imageResampler.cpp","$Repo/UniqueLibs/document/imageColor.hpp","$Repo/HyperEditor/src/image/QImageView.cpp","$Repo/UniqueLibs/document/documentStorageAdapter.cpp","$Repo/UniqueLibs/document/modernImagePlugin.cpp","$Root/native/Release/hc_image_comparison.exe","$Root/avif-yuv-build/Release/avif.dll" | Select-Object Path,Hash)
    boundaries=@('File OS cache is not flushed. App cache is explicitly cleared where stated.',
        'Component kernel results do not include document host IPC or F3 materialization.',
        'GPU results include transfer, synchronization and CPU result download; startup is reported separately.',
        'Resize inputs are canonical RGBA8. Arbitrary ICC/HDR GPU integration remains outside this prototype.',
        'Unequal kernels, gamma, alpha, orientation and bit-depth policies must not be ranked as equal-quality alternatives.')
}
[IO.File]::WriteAllText("$Root/environment.json",($Report|ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
