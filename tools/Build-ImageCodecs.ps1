param(
    [string]$BuildRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) '_build/image-codecs'),
    [string]$QtKit = 'D:/Qt/Qt-6.12.0-VC2026-X64-H-MD-2026-10-10'
)
$ErrorActionPreference = 'Stop'
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
$Prefix = Join-Path $BuildRoot 'runtime'
$Sources = Join-Path $BuildRoot 'sources'
New-Item -ItemType Directory -Force $BuildRoot,$Prefix,$Sources | Out-Null
$Lock = @(
    @('libde265','1.1.3','https://codeload.github.com/strukturag/libde265/tar.gz/refs/tags/v1.1.3','189baa08fd6d2dd34db099de411bd6b2e6bd5eb88e81236a3d0fa3826b9715c4'),
    @('libheif','1.23.6','https://codeload.github.com/strukturag/libheif/tar.gz/refs/tags/v1.23.6','c9a00b0452f5c16d72ef71e5e40c0e90fbe2ad37148d27a1b2f9bb21546f5f03'),
    @('dav1d','1.5.3','https://downloads.videolan.org/pub/videolan/dav1d/1.5.3/dav1d-1.5.3.tar.xz','732010aa5ef461fa93355ed2c6c5fedb48ddc4b74e697eaabe8907eaeb943011'),
    @('libavif','1.4.2','https://codeload.github.com/AOMediaCodec/libavif/tar.gz/refs/tags/v1.4.2','2b645287340ba5a631d268b551dc2d72bd73ac33335962dd36dcdb6d8366921d'),
    @('libyuv','644251f25','https://chromium.googlesource.com/libyuv/libyuv/+archive/644251f25.tar.gz','4c730925a94e826b2add3d74ab35cdb71d78bd62d64945196ba3b5bae668ea88','file-tree-sha256-v1')
)
$Recipe = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash + '|' + $QtKit
$Stamp = Join-Path $BuildRoot 'complete.txt'
if ((Test-Path $Stamp) -and ([IO.File]::ReadAllText($Stamp) -eq $Recipe) -and
    (Test-Path "$Prefix/bin/heif.dll") -and (Test-Path "$Prefix/bin/avif.dll")) { return }
function Run([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Image codec build failed ($LASTEXITCODE): $Exe $Arguments" }
}
$Vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$Vs = & $Vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $Vs) { throw 'Visual Studio C++ tools are required.' }
$Setup = Join-Path $Vs 'Common7/Tools/VsDevCmd.bat'
$Environment = & $env:ComSpec /d /c "call `"$Setup`" -no_logo -arch=amd64 -host_arch=amd64 && set"
foreach ($Line in $Environment) {
    if ($Line -match '^(PATH|INCLUDE|LIB|LIBPATH|VCToolsInstallDir|WindowsSdkDir|VCINSTALLDIR|DevEnvDir)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
$env:PATH = "$QtKit/bin;D:/Qt/Tools/Ninja;$env:PATH"
$env:CC = 'cl'
$env:CXX = 'cl'
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) { throw 'MSVC environment initialization did not expose cl.exe.' }
$Python = (& uv python find 3.12 --offline).Trim()
if ($LASTEXITCODE) { throw 'Python 3.12 is required for source verification and Meson.' }
foreach ($Item in $Lock) {
    $Ext = if ($Item[0] -eq 'dav1d') { '.tar.xz' } else { '.tar.gz' }
    $Archive = Join-Path $Sources ($Item[0] + '-' + $Item[1] + $Ext)
    if (-not (Test-Path $Archive)) { Invoke-WebRequest -UseBasicParsing $Item[2] -OutFile $Archive }
    if ($Item[0] -eq 'libyuv') {
        Run $Python @((Join-Path $PSScriptRoot 'Verify-ImageCodecSource.py'),$Archive,$Item[3])
    } elseif ((Get-FileHash $Archive -Algorithm SHA256).Hash -ne $Item[3]) { throw "Codec source hash mismatch: $Archive" }
    $Source = Join-Path $Sources ($Item[0] + '-' + $Item[1])
    if (-not (Test-Path $Source)) {
        if ($Item[0] -eq 'libyuv') {
            New-Item -ItemType Directory -Force $Source | Out-Null
            Run 'tar' @('-xf',$Archive,'-C',$Source)
        } else { Run 'tar' @('-xf',$Archive,'-C',$Sources) }
    }
    $Notices = Join-Path $Prefix ('Licenses/' + $Item[0])
    New-Item -ItemType Directory -Force $Notices | Out-Null
    Copy-Item -LiteralPath $Archive -Destination $Notices
    Get-ChildItem (Join-Path $Sources ($Item[0] + '-' + $Item[1])) -File |
        Where-Object Name -Match '^(COPYING|LICENSE|PATENTS|AUTHORS|NOTICE)' | Copy-Item -Destination $Notices
}
$QtCmake = Join-Path $QtKit 'bin/qt-cmake.bat'
$Common = @('-G','Visual Studio 18 2026','-A','x64','-T','v145,host=x64',
    "-DCMAKE_INSTALL_PREFIX=$Prefix",'-DCMAKE_POLICY_DEFAULT_CMP0091=NEW','-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
    '-DCMAKE_SHARED_LINKER_FLAGS=/NODEFAULTLIB:libucrt.lib /DEFAULTLIB:ucrt.lib',
    '-DBUILD_SHARED_LIBS=ON','-DBUILD_TESTING=OFF')
function CmakeBuild([string]$Name,[string]$Version,[string[]]$Options) {
    $Source = Join-Path $Sources "$Name-$Version"
    $Build = Join-Path $BuildRoot "$Name-build"
    Run $QtCmake (@('-S',$Source,'-B',$Build) + $Common + $Options)
    Run $QtCmake @('--build',$Build,'--config','Release','--target','install','--parallel','6')
}
CmakeBuild 'libde265' '1.1.3' @('-DENABLE_SDL=OFF','-DENABLE_DECODER=OFF','-DENABLE_ENCODER=OFF','-DENABLE_AVX512=OFF')
$HeifOptions = @('-DWITH_EXAMPLES=OFF','-DWITH_GDK_PIXBUF=OFF','-DBUILD_DOCUMENTATION=OFF',
    '-DENABLE_PLUGIN_LOADING=OFF',"-DCMAKE_PREFIX_PATH=$Prefix",'-DWITH_LIBDE265=ON',
    "-DLIBDE265_INCLUDE_DIR=$Prefix/include","-DLIBDE265_LIBRARY=$Prefix/lib/de265.lib")
$HeifSource = [IO.File]::ReadAllText((Join-Path $Sources 'libheif-1.23.6/CMakeLists.txt'))
foreach ($Match in [regex]::Matches($HeifSource,'plugin_option\(([A-Za-z0-9_]+)\s')) {
    if ($Match.Groups[1].Value -ne 'LIBDE265') { $HeifOptions += '-DWITH_' + $Match.Groups[1].Value + '=OFF' }
}
CmakeBuild 'libheif' '1.23.6' $HeifOptions
$env:UV_CACHE_DIR = Join-Path $BuildRoot 'uv-cache'
$Python = (& uv python find).Trim()
if ($LASTEXITCODE -ne 0) { throw 'A Python installation is required for Meson.' }
$Packages = Join-Path $BuildRoot 'python-packages'
Run 'uv' @('pip','install','--python',$Python,'--target',$Packages,'meson==1.12.1')
$env:PYTHONPATH = $Packages
$env:PYTHONDONTWRITEBYTECODE = '1'
$Native = Join-Path $BuildRoot 'hybrid-crt.ini'
@"
[binaries]
c = 'cl'
cpp = 'cl'
[built-in options]
b_vscrt = 'mt'
c_link_args = ['/NODEFAULTLIB:libucrt.lib', '/DEFAULTLIB:ucrt.lib']
"@ | Set-Content -Encoding ASCII $Native
$DavBuild = Join-Path $BuildRoot 'dav1d-build'
if (-not (Test-Path "$DavBuild/build.ninja")) {
    Run $Python @('-m','mesonbuild.mesonmain','setup',$DavBuild,(Join-Path $Sources 'dav1d-1.5.3'),
        '--buildtype=release','--default-library=shared',"--prefix=$Prefix",'--libdir=lib',
        '--native-file',$Native,'-Denable_tools=false','-Denable_examples=false','-Denable_tests=false','-Denable_asm=true')
}
Run $Python @('-m','mesonbuild.mesonmain','compile','-C',$DavBuild,'-j','6')
Run $Python @('-m','mesonbuild.mesonmain','install','-C',$DavBuild)
$ImportDav1d = Join-Path (Split-Path $PSScriptRoot -Parent) 'cmake/ImageCodecDav1d.cmake'
$YuvSource = Join-Path $Sources 'libyuv-644251f25'
$YuvBuild = Join-Path $BuildRoot 'libyuv-build'
# Build only the static library. libavif carries the conversion code, so no new
# runtime DLL or upstream command-line programs are deployed.
Run $QtCmake (@('-S',$YuvSource,'-B',$YuvBuild) + $Common + @('-DUNIT_TEST=OFF'))
Run $QtCmake @('--build',$YuvBuild,'--config','Release','--target','yuv','--parallel','6')
Copy-Item -LiteralPath "$YuvBuild/Release/yuv.lib" -Destination "$Prefix/lib/yuv.lib" -Force
Copy-Item -Path "$YuvSource/include/*" -Destination "$Prefix/include" -Recurse -Force
CmakeBuild 'libavif' '1.4.2' @('-DAVIF_CODEC_DAV1D=SYSTEM','-DAVIF_LIBYUV=SYSTEM','-DAVIF_BUILD_APPS=OFF',
    "-DLIBYUV_INCLUDE_DIR=$Prefix/include","-DLIBYUV_LIBRARY=$Prefix/lib/yuv.lib",
    '-DAVIF_BUILD_TESTS=OFF',"-DHC_IMAGE_CODEC_PREFIX=$Prefix","-DCMAKE_PROJECT_INCLUDE=$ImportDav1d")
[IO.File]::WriteAllText((Join-Path $Prefix 'Licenses/image-codecs-source-lock.json'),($Lock | ConvertTo-Json -Depth 4))
[IO.File]::WriteAllText($Stamp,$Recipe)
