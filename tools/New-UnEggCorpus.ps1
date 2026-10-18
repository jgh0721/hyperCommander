param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$Revision = '78b64f54026a8687dac1cdd4c86eb5fe5abf04ae'
$Base = 'https://raw.githubusercontent.com/akolman/EggDotNet/' + $Revision + '/'
$Names = @('defaults.alz','defaults.egg','defaults_nocompress.egg','defaults_normal.egg','defaults_bz_optimized.egg',
    'default_lzma_pri_compress_ratio.egg','solid.egg','solid_defaults.egg','azomulti.egg','azonumbers.egg','azoshort_txt.egg',
    'lzma_simple.egg','small_kor.egg','small_shift.egg','lorem_long_aes128.egg','lorem_long_aes256.egg',
    'lorem_long_store_lea128.egg','lorem_long_store_lea256.egg','lorem_long_zipEnc.egg','number.vol1.egg','number.vol2.egg',
    'directories.egg','lorem_short_readonlyhidden.egg','basic_deflate_folders.egg')
$ManifestPath = Join-Path $OutputDirectory 'corpus.json'
if (Test-Path -LiteralPath $ManifestPath) {
    $Saved = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($Item in $Saved.files) {
        if ((Get-FileHash -LiteralPath (Join-Path $OutputDirectory $Item.name)).Hash -ne $Item.sha256) { throw 'UnEGG corpus hash mismatch' }
    }
    [IO.File]::SetLastWriteTimeUtc($ManifestPath, [DateTime]::UtcNow)
    Write-Output "Reused verified corpus: $ManifestPath"
    return
}
$Locks = @()
foreach ($Name in @('LICENSE.txt') + $Names) {
    $Url = if ($Name -eq 'LICENSE.txt') { $Base + $Name } else { $Base + 'src/EggDotNet.Tests/test_files/' + $Name }
    $Path = Join-Path $OutputDirectory $Name
    if (-not (Test-Path -LiteralPath $Path)) { Invoke-WebRequest $Url -OutFile $Path -UseBasicParsing }
    $Locks += @{name=$Name; source=$Url; sha256=(Get-FileHash -LiteralPath $Path).Hash}
}
$Cases = foreach ($Name in $Names) {
    if ($Name -eq 'number.vol2.egg') { continue }
    @{archive=(Join-Path $OutputDirectory $Name); password=$(if ($Name -match '(aes|lea|zipEnc)') { 'password12345!' } else { '' })}
}
# Published expected SHA256 values from the upstream unit test; these are data,
# not an extraction result produced by the engine under test.
$Expected = @{
    'lorem_ipsum_short.txt'='ACFE59AFCFFCBE68A4DEE454D11F5BF78DFDEE32C77A03C1A50CF384418FE38F'
    'lorem_ipsum_long.pdf'='4B3E49290B2399C2AB63886B0585B819AE7C6D9FA947F1B3DB53BD9DA62A2B10'
    'lorem_ipsum_long.tif'='162D8B99FD7E695BD3E8C790493C93B649A76996F4E129C818A366614977EC67'
    'lorem_ipsum_medium.txt'='5DEA92AB15A3A73AF46B0AFBF793C109B576E35C62CE9DB703E7F4088F2E8A07'
    'lorem_ipsum_long.txt'='D63CFF6C64AB12F1C08A298F7B3A0C27F19A9822D2C6C7156D676D62265DB46E'
    'lorem_ipsum.zip'='F7E4474594F6B6C8EB4A38A86C5391761AC8C8B8F8CFE2D35101A1B43B467BCC'
}
$Json = @{upstream=$Revision; files=$Locks; cases=@($Cases); expected=$Expected} | ConvertTo-Json -Depth 6
[IO.File]::WriteAllText($ManifestPath, ($Json -replace "`r?`n", "`r`n"), [Text.UTF8Encoding]::new($false))
Write-Output "Generated $(@($Cases).Count) cases: $OutputDirectory"
