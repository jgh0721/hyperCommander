param(
    [string]$Root = (Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) '_build/image-performance'),
    [switch]$SkipPackages
)
$ErrorActionPreference = 'Stop'
$Root = [IO.Path]::GetFullPath($Root)
New-Item -ItemType Directory -Force $Root,"$Root/vendor","$Root/results","$Root/corpus" | Out-Null
$env:UV_CACHE_DIR = "$Root/uv-cache"
$Python = (& uv python find 3.12 --offline).Trim()
if ($LASTEXITCODE) { throw 'Python 3.12 is required.' }
$Packages = @('numpy==2.2.6','pillow==11.3.0','opencv-python-headless==4.12.0.88',
    'scipy==1.16.1','scikit-image==0.25.2','pyvips==3.0.0','wand==0.6.13',
    'psutil==7.0.0','pillow-heif==1.1.0','imagecodecs==2025.8.2')
if (-not $SkipPackages) {
& uv pip install --python $Python --target "$Root/python" @Packages
if ($LASTEXITCODE) { throw 'Comparison dependencies failed.' }
& uv pip install --python $Python --target "$Root/extra-python" --no-deps 'OpenImageIO==3.2.1.3' 'skia-python==144.0.post2'
if ($LASTEXITCODE) { throw 'Additional comparison dependencies failed.' }
}
$Downloads = @(
    @('zimg.zip','https://codeload.github.com/sekrit-twc/zimg/zip/refs/tags/release-3.0.6','1AE4D5F3BA154D982270F672772DB00443655DCD8F8CECD6106403EC20C194AA'),
    @('stb_image_resize2.h','https://raw.githubusercontent.com/nothings/stb/a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d/deprecated/stb_image_resize2.h','771C1EB7193FE0682F7CADD6040E906CE625D32CBA311E2274C6B09A2875179E'),
    @('vips.zip','https://github.com/libvips/build-win64-mxe/releases/download/v8.18.7/vips-dev-x64-all-8.18.7.zip','78E56BA30C267BA32FF965200534BD5721364CADBEAD8C4D595E44B58DFEFF15'),
    @('simd.zip','https://codeload.github.com/ermig1979/Simd/zip/refs/tags/v8.0.166','F45161668B217A7DAC429BC5AB45E1A993721A4CD59684E44E1F96893C0ACD9E'),
    @('libyuv.tar.gz','https://chromium.googlesource.com/libyuv/libyuv/+archive/644251f25.tar.gz','4c730925a94e826b2add3d74ab35cdb71d78bd62d64945196ba3b5bae668ea88')
)
$Manifest = @()
foreach ($D in $Downloads) {
    $Path = Join-Path "$Root/vendor" $D[0]
    if (!(Test-Path -LiteralPath $Path)) { Invoke-WebRequest -UseBasicParsing $D[1] -OutFile $Path }
    if ($D[0] -eq 'libyuv.tar.gz') {
        & $Python (Join-Path (Split-Path $PSScriptRoot -Parent) 'Verify-ImageCodecSource.py') $Path $D[2]
        if ($LASTEXITCODE) { throw 'Comparison libyuv source verification failed.' }
    } elseif ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $D[2]) { throw "Comparison source hash mismatch: $Path" }
    $Manifest += @{name=$D[0];url=$D[1];sha256=(Get-FileHash -LiteralPath $Path).Hash}
    if ($D[0].EndsWith('.zip') -and !(Test-Path "$Path.extracted")) {
        Expand-Archive -LiteralPath $Path -DestinationPath "$Root/vendor" -Force
        [IO.File]::WriteAllText("$Path.extracted",'ok')
    }
    if ($D[0] -eq 'libyuv.tar.gz' -and !(Test-Path "$Root/vendor/libyuv/CMakeLists.txt")) {
        New-Item -ItemType Directory -Force "$Root/vendor/libyuv" | Out-Null
        & tar -xf $Path -C "$Root/vendor/libyuv"
        if ($LASTEXITCODE) { throw 'libyuv extraction failed.' }
    }
}
# Freeze the pre-optimization implementation for reproducible numerical and timing comparisons.
$Repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Before = (& git -C $Repo show 'cde2bad:UniqueLibs/document/imageResampler.cpp') -join "`n"
if ($LASTEXITCODE) { throw 'The cde2bad comparison baseline must be available.' }
$Start = $Before.IndexOf('    ImageRenderResult ResampleImage(')
if ($Start -lt 0) { throw 'Comparison baseline signature changed.' }
$Reference = "#include `"stdafx.h`"`n#include `"UniqueLibs/document/imageResampler.hpp`"`n#include <QMutex>`n#include <mutex>`n#include <array>`n#include <cmath>`n#include <cstring>`nnamespace hc::document { namespace { QMutex RenderMutex; }`n" + $Before.Substring($Start).Replace('ResampleImage(', 'ResampleReference(')
[IO.File]::WriteAllText("$Root/reference.cpp",($Reference -replace "`r?`n","`r`n"),[Text.UTF8Encoding]::new($true))
[IO.File]::WriteAllText("$Root/dependencies.json",($Manifest | ConvertTo-Json -Depth 5))
[IO.File]::WriteAllText("$Root/python-path.txt",$Python)
