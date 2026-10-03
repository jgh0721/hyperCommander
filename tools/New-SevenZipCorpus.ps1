param([string]$OutputDirectory = '_build/sevenzip-corpus', [string]$ToolsDirectory = '_build/core7')
$ErrorActionPreference = 'Stop'
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$ToolsDirectory = [IO.Path]::GetFullPath($ToolsDirectory)
$ManifestPath = Join-Path $OutputDirectory 'corpus.json'
if (Test-Path -LiteralPath $ManifestPath) {
    $Saved = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($Item in $Saved.rar) {
        if ((Get-FileHash -LiteralPath (Join-Path $OutputDirectory $Item.name)).Hash -ne $Item.sha256) { throw 'RAR corpus hash mismatch' }
    }
    foreach ($Item in $Saved.cases) { if (-not (Test-Path -LiteralPath $Item.archive) -or -not (Test-Path -LiteralPath $Item.expected)) { throw 'Incomplete corpus' } }
    [IO.File]::SetLastWriteTimeUtc($ManifestPath, [DateTime]::UtcNow)
    Write-Output "Reused verified corpus: $ManifestPath"
    return
}
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
[IO.Directory]::CreateDirectory($ToolsDirectory) | Out-Null
function Fetch($Name, $Hash) {
    $Path = Join-Path $ToolsDirectory $Name
    $Url = if ($Name -eq '7zr.exe') { 'https://github.com/ip7z/7zip/releases/download/26.03/7zr.exe' } else { 'https://www.7-zip.org/a/' + $Name }
    if (-not (Test-Path -LiteralPath $Path)) { Invoke-WebRequest $Url -OutFile $Path -UseBasicParsing }
    if ((Get-FileHash -LiteralPath $Path).Hash -ne $Hash) { throw "7-Zip tool hash mismatch: $Name" }
    return $Path
}
$Bootstrap = Fetch '7zr.exe' 'AD4C82FADCBDF93C03B4FC440F300509C7D60C5C2F4D183E35D9D70D6957037D'
$Installer = Fetch '7z2603-x64.exe' '0859C524B8A63551848F0C246ABDDCB1D0B7B656B0FBFE879F8D85E61A9E6EDD'
$Official = Join-Path $ToolsDirectory 'official'
& $Bootstrap x $Installer ('-o' + $Official) -y | Out-Null
if ($LASTEXITCODE) { throw 'Official CLI extraction failed' }
$Cli = Join-Path $Official '7z.exe'
$InputDir = Join-Path $OutputDirectory 'input'
[IO.Directory]::CreateDirectory($InputDir) | Out-Null
[IO.Directory]::CreateDirectory((Join-Path $InputDir 'sub')) | Out-Null
$Bytes = [byte[]]::new(262144); [Random]::new(712).NextBytes($Bytes)
[IO.File]::WriteAllBytes((Join-Path $InputDir 'payload.bin'), $Bytes)
[IO.File]::WriteAllBytes((Join-Path $InputDir 'empty.bin'), [byte[]]::new(0))
[IO.File]::WriteAllBytes((Join-Path $InputDir 'sub/zeros.bin'), [byte[]]::new(8388608))
[IO.File]::WriteAllText((Join-Path $InputDir ('sub/' + [char]0xD55C + [char]0xAE00 + '.txt')), 'unicode-content')
$Cases = [Collections.Generic.List[object]]::new()
$Recipes = @(
    @{name='plain.7z'; args=@('-t7z','-mx=1','-ms=off')},
    @{name='solid.7z'; args=@('-t7z','-mx=1','-ms=on')},
    @{name='encrypted.7z'; args=@('-t7z','-mx=1','-psecret','-mhe=on'); password='secret'},
    @{name='plain.zip'; args=@('-tzip','-mx=1')},
    @{name='aes.zip'; args=@('-tzip','-mx=1','-mem=AES256','-psecret'); password='secret'},
    @{name='zipcrypto.zip'; args=@('-tzip','-mx=1','-mem=ZipCrypto','-psecret'); password='secret'},
    @{name='split.7z'; args=@('-t7z','-mx=1','-v128k'); volume=$true}
)
foreach ($Recipe in $Recipes) {
    $Archive = Join-Path $OutputDirectory $Recipe.name
    if (Test-Path -LiteralPath $Archive) { throw "Use a fresh corpus directory: $Archive" }
    Push-Location $InputDir
    try { & $Cli a $Archive @($Recipe.args) '*' -y | Out-Null; if ($LASTEXITCODE) { throw 'Fixture generation failed' } } finally { Pop-Location }
    if ($Recipe.volume) { $Archive += '.001' }
    $Cases.Add(@{archive=$Archive; password=[string]$Recipe.password; expected=$InputDir})
}
$Revision = '7f53fce04e4e672230f4eb80b219af17975e4f83'
$Base = 'https://raw.githubusercontent.com/libarchive/libarchive/' + $Revision + '/libarchive/test/'
$Names = @('test_read_format_rar5_compressed.rar', 'test_read_format_rar5_stored.rar', 'test_read_format_rar5_multiple_files_solid.rar',
    'test_read_format_rar4_solid_encrypted.rar', 'test_read_format_rar4_encrypted_filenames.rar',
    'test_read_format_rar5_solid_encrypted.rar', 'test_read_format_rar5_encrypted_filenames.rar')
$Names += 1..4 | ForEach-Object { 'test_read_format_rar_multivolume.part{0:d4}.rar' -f $_ }
$Names += 1..8 | ForEach-Object { 'test_read_format_rar5_multiarchive.part{0:d2}.rar' -f $_ }
$Names += 1..4 | ForEach-Object { 'test_read_format_rar5_multiarchive_solid.part{0:d2}.rar' -f $_ }
$Locks = [Collections.Generic.List[object]]::new()
foreach ($Name in $Names) {
    $Path = Join-Path $OutputDirectory $Name
    $Encoded = $Path + '.uu'
    Invoke-WebRequest ($Base + $Name + '.uu') -OutFile $Encoded -UseBasicParsing
    $Out = [Collections.Generic.List[byte]]::new()
    foreach ($Line in [IO.File]::ReadAllLines($Encoded)) {
        if (-not $Line -or $Line.StartsWith('begin ') -or $Line -eq 'end') { continue }
        $Count = ([int][char]$Line[0] - 32) -band 63
        for ($I = 1; $Count -gt 0; $I += 4) {
            $A = ([int][char]$Line[$I] - 32) -band 63; $B = ([int][char]$Line[$I+1] - 32) -band 63
            $C = ([int][char]$Line[$I+2] - 32) -band 63; $D = ([int][char]$Line[$I+3] - 32) -band 63
            foreach ($Value in @((($A -shl 2) -bor ($B -shr 4)), ((($B -shl 4) -bor ($C -shr 2)) -band 255), ((($C -shl 6) -bor $D) -band 255))) {
                if ($Count -gt 0) { $Out.Add([byte]$Value); $Count-- }
            }
        }
    }
    [IO.File]::WriteAllBytes($Path, $Out.ToArray())
    $Locks.Add(@{name=$Name; sha256=(Get-FileHash $Path).Hash; source=$Base + $Name + '.uu'})
}
foreach ($Name in $Names) {
    if ($Name -match '\.part' -and $Name -notmatch '\.part0*1\.rar$') { continue }
    $Password = if ($Name -match 'encrypted') { 'password' } else { '' }
    $Expected = Join-Path $OutputDirectory ('expected-' + $Name)
    # Compare regular payloads; link creation is deliberately unsupported by HC.
    & $Cli x (Join-Path $OutputDirectory $Name) ('-o' + $Expected) ('-p' + $Password) '-x!testlink' -y | Out-Null
    if ($LASTEXITCODE) { throw "Official CLI rejected corpus: $Name ($LASTEXITCODE)" }
    $Cases.Add(@{archive=(Join-Path $OutputDirectory $Name); password=$Password; expected=$Expected})
}
$Json = @{cases=$Cases.ToArray(); upstream=$Revision; rar=$Locks.ToArray() } | ConvertTo-Json -Depth 6
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'corpus.json'), ($Json -replace "`r?`n", "`r`n"), [Text.UTF8Encoding]::new($false))
Write-Output "Generated $($Cases.Count) cases: $OutputDirectory"
