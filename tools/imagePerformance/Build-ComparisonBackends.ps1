param(
    [string]$Root = (Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) '_build/image-performance'),
    [string]$QtKit = 'D:/Qt/Qt-6.12.0-VC2026-X64-H-MD-2026-10-10'
)
$ErrorActionPreference='Stop'
$Root=[IO.Path]::GetFullPath($Root)
$Repo=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Codec="$Repo/_build/image-codecs"
$Cmake="$QtKit/bin/qt-cmake.bat"
function Run([string[]]$Arguments){
    & $Cmake @Arguments
    if($LASTEXITCODE){throw "Comparison build failed: $Arguments"}
}
if(!(Test-Path "$Root/reference.cpp") -or !(Test-Path "$Codec/runtime/bin/avif.dll")){
    throw 'Run Build-ImageCodecs.ps1 and Prepare-ImageComparison.ps1 first.'
}
$Common=@('-G','Visual Studio 18 2026','-A','x64','-T','v145,host=x64',
    '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW','-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
    '-DCMAKE_SHARED_LINKER_FLAGS=/NODEFAULTLIB:libucrt.lib /DEFAULTLIB:ucrt.lib')
Run (@('-S',"$Root/vendor/Simd-8.0.166/prj/cmake",'-B',"$Root/simd-build")+$Common+@(
    '-DSIMD_TARGET=AMD64','-DSIMD_TOOLCHAIN=','-DSIMD_SHARED=ON','-DSIMD_TEST=OFF','-DSIMD_SYNET=OFF',
    '-DSIMD_PYTHON=OFF','-DSIMD_AVX512=OFF','-DSIMD_AVX512VNNI=OFF',
    "-DCMAKE_PROJECT_INCLUDE=$Repo/tests/imagePerformance/simdProject.cmake"))
Run @('--build',"$Root/simd-build",'--config','Release','--parallel','6')
Run (@('-S',"$Root/vendor/libyuv",'-B',"$Root/yuv-build")+$Common+@('-DUNIT_TEST=OFF'))
Run @('--build',"$Root/yuv-build",'--config','Release','--target','yuv','--parallel','6')
Run (@('-S',"$Codec/sources/libavif-1.4.2",'-B',"$Root/avif-yuv-build")+$Common+@(
    '-DBUILD_SHARED_LIBS=ON','-DAVIF_CODEC_DAV1D=SYSTEM','-DAVIF_LIBYUV=SYSTEM',
    '-DAVIF_BUILD_APPS=OFF','-DAVIF_BUILD_TESTS=OFF',"-DHC_IMAGE_CODEC_PREFIX=$Codec/runtime",
    "-DCMAKE_PROJECT_INCLUDE=$Repo/cmake/ImageCodecDav1d.cmake",
    "-DLIBYUV_INCLUDE_DIR=$Root/vendor/libyuv/include","-DLIBYUV_LIBRARY=$Root/yuv-build/Release/yuv.lib"))
Run @('--build',"$Root/avif-yuv-build",'--config','Release','--parallel','6')
Run (@('-S',"$Repo/tests/imagePerformance",'-B',"$Root/native","-DHC_PERF_ROOT=$Root")+$Common)
Run @('--build',"$Root/native",'--config','Release','--parallel','6')
