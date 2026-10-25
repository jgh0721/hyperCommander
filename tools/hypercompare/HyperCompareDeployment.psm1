# Shared-root component installer. The host product keeps ownership of its runtime.
# Import-Module ./tools/hypercompare/HyperCompareDeployment.psm1
# Install-HyperCompare -SourceDirectory <signed-package> -Destination D:/Utils/HyperCommander
Import-Module (Join-Path $PSScriptRoot '../update/ProductSigning.psm1') -DisableNameChecking

function Assert-HyperCompareManifest($Manifest) {
    if ($Manifest.schemaVersion -ne 1 -or $Manifest.product -ne 'HyperCompare' -or $Manifest.configuration -ne 'Release' -or @($Manifest.files).Count -lt 8 -or @($Manifest.files).Count -gt 100) { throw 'Invalid HyperCompare inventory.' }
    $Seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($Entry in $Manifest.files) {
        if ($Entry.path -isnot [string] -or $Entry.path -cnotmatch '^(HyperCompare(?:Cli)?\.exe|Qt6(?:Core|Gui|Widgets|Network|Svg)\.dll|Libs/platforms/qwindows\.dll|Libs/iconengines/qsvgicon\.dll|Libs/imageformats/qsvg\.dll|Licenses/[A-Za-z0-9_.-]+\.txt)$' -or -not $Seen.Add($Entry.path) -or $Entry.sha256 -notmatch '^[a-fA-F0-9]{64}$') { throw 'Invalid HyperCompare inventory entry.' }
    }
}

function Test-HyperComparePackage {
    param([Parameter(Mandatory=$true)][string]$Root)
    $ErrorActionPreference = 'Stop'
    $Root = [IO.Path]::GetFullPath($Root)
    Assert-UpdatePlainPath $Root
    $ManifestPath = Join-Path $Root 'hypercompare-inventory.json'
    Assert-UpdatePlainPath $ManifestPath
    $Manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-HyperCompareManifest $Manifest
    if ($Manifest.phase -ne 'foundation') { throw 'Unexpected HyperCompare package phase.' }
    $Seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($Entry in $Manifest.files) {
        $Relative = [string]$Entry.path
        if ($Relative -cnotmatch '^(HyperCompare(?:Cli)?\.exe|Qt6(?:Core|Gui|Widgets|Network|Svg)\.dll|Libs/platforms/qwindows\.dll|Libs/iconengines/qsvgicon\.dll|Libs/imageformats/qsvg\.dll|Licenses/[A-Za-z0-9_.-]+\.txt)$' -or
            -not $Seen.Add($Relative) -or $Entry.sha256 -notmatch '^[a-fA-F0-9]{64}$') { throw "Invalid package entry: $Relative" }
        $File = Join-Path $Root $Relative; Assert-UpdatePlainPath $File
        if ((Get-Item -LiteralPath $File).Length -ne $Entry.size -or (Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash -ine $Entry.sha256) { throw "Package hash/size mismatch: $Relative" }
        if ($Relative -match '\.(exe|dll)$') { $null = Assert-UpdateSignature $File $Relative }
}
foreach ($Required in @('HyperCompare.exe','HyperCompareCli.exe','Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll','Qt6Svg.dll','Libs/platforms/qwindows.dll','Licenses/Scintilla.txt','Licenses/Lexilla.txt','Licenses/waterColorStyle.txt','Licenses/hc_tree_sitter.txt','Licenses/hc_tree_sitter_c.txt','Licenses/hc_tree_sitter_cpp.txt','Licenses/hc_tree_sitter_python.txt','Licenses/miniz.txt','Licenses/pcre2.txt')) {
    if (-not $Seen.Contains($Required)) { throw "Required package entry missing: $Required" }
}
# Keep unlisted DLLs and executables out of the package's loader search path.
foreach ($File in Get-ChildItem -LiteralPath $Root -Recurse -File) {
    $Relative = $File.FullName.Substring($Root.TrimEnd('\').Length + 1).Replace('\','/')
    if ($Relative -ne 'hypercompare-inventory.json' -and -not $Seen.Contains($Relative)) { throw "Unlisted package file: $Relative" }
}
return $Manifest

}

function Install-HyperCompare {
    param([Parameter(Mandatory=$true)][string]$SourceDirectory, [string]$Destination = 'D:\Utils\HyperCommander')
    $ErrorActionPreference = 'Stop'
    $Repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
    $SourceDirectory = [IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\')
    $Destination = [IO.Path]::GetFullPath($Destination).TrimEnd('\')
    if ($Destination -eq [IO.Path]::GetPathRoot($Destination).TrimEnd('\') -or $Destination -eq $Repo -or $Destination.StartsWith($Repo + '\',[StringComparison]::OrdinalIgnoreCase) -or
        $Destination -eq $SourceDirectory -or $SourceDirectory.StartsWith($Destination + '\',[StringComparison]::OrdinalIgnoreCase) -or $Destination.StartsWith($SourceDirectory + '\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Use an installation directory outside the checkout and source package.' }
    $Manifest = Test-HyperComparePackage -Root $SourceDirectory
    Assert-UpdatePlainPath $Destination
    foreach ($Process in Get-Process) {
        if ($Process.Path -and $Process.ProcessName -in @('HyperCompare','HyperCompareCli') -and $Process.Path.StartsWith($Destination + '\',[StringComparison]::OrdinalIgnoreCase)) { throw "Close HyperCompare before updating: $($Process.Id)" }
}
$ManifestName = 'hypercompare-inventory.json'
$Previous = @{}
if (Test-Path -LiteralPath (Join-Path $Destination $ManifestName)) {
    Assert-UpdatePlainPath (Join-Path $Destination $ManifestName)
    $Old = Get-Content -LiteralPath (Join-Path $Destination $ManifestName) -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-HyperCompareManifest $Old
    foreach ($Entry in $Old.files) { $Previous[[string]$Entry.path] = [string]$Entry.sha256 }
}
# A component must never replace a different runtime owned by the host product.
$HostFiles = @{}
$HostInventory = Join-Path $Destination 'product-inventory.json'
if (Test-Path -LiteralPath $HostInventory) {
    Assert-UpdatePlainPath $HostInventory
    $HostManifest = Get-Content -LiteralPath $HostInventory -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($HostManifest.product -ne 'HyperCommander') { throw 'Unexpected host product identity.' }
    foreach ($Entry in $HostManifest.files) { $HostFiles[[string]$Entry.path] = [string]$Entry.sha256 }
}
$Files = @($Manifest.files) + @([pscustomobject]@{path=$ManifestName;sha256=(Get-FileHash -LiteralPath (Join-Path $SourceDirectory $ManifestName) -Algorithm SHA256).Hash})
# Validate every destination before the first write. Unowned/modified files are preserved.
foreach ($Entry in $Files) {
    $Relative = [string]$Entry.path; $Target = Join-Path $Destination $Relative
    Assert-UpdatePlainPath $Target
    if ($HostFiles.ContainsKey($Relative) -and $HostFiles[$Relative] -ine $Entry.sha256) { throw "Shared host runtime differs: $Relative" }
    if (Test-Path -LiteralPath $Target) {
        $Actual = (Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash
        if ($HostFiles.ContainsKey($Relative) -and $Actual -ine $Entry.sha256) { throw "Modified host runtime: $Relative" }
        if ($Actual -ine $Entry.sha256 -and $Relative -ne $ManifestName -and
            (-not $Previous.ContainsKey($Relative) -or $Previous[$Relative] -ine $Actual)) { throw "Preserved unowned or modified file: $Relative" }
    }
}
$Legacy = Join-Path $Destination 'HyperCompare'
$LegacyInventory = Join-Path $Legacy $ManifestName
$LegacyManifest = $null
if (Test-Path -LiteralPath $LegacyInventory) {
    Assert-UpdatePlainPath $LegacyInventory
    $LegacyManifest = Get-Content -LiteralPath $LegacyInventory -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-HyperCompareManifest $LegacyManifest
    foreach ($Entry in $LegacyManifest.files) { Assert-UpdatePlainPath (Join-Path $Legacy ([string]$Entry.path)) }
}
$Evidence = Join-Path $Repo ('_build/hypercompare-install/' + [Guid]::NewGuid().ToString('N'))
$Changed = [Collections.Generic.List[object]]::new()
try {
    foreach ($Entry in $Files) {
        $Relative = [string]$Entry.path; $InputFile = Join-Path $SourceDirectory $Relative; $Target = Join-Path $Destination $Relative
        Assert-UpdatePlainPath $Target
        if ((Get-FileHash -LiteralPath $InputFile -Algorithm SHA256).Hash -ine $Entry.sha256) { throw "Source changed: $Relative" }
        if ((Test-Path -LiteralPath $Target) -and (Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash -ieq $Entry.sha256) { continue }
        [IO.Directory]::CreateDirectory((Split-Path $Target -Parent)) | Out-Null
        $Temporary = $Target + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
        try {
            Copy-Item -LiteralPath $InputFile -Destination $Temporary
            if ((Get-FileHash -LiteralPath $Temporary -Algorithm SHA256).Hash -ine $Entry.sha256) { throw "Staged hash mismatch: $Relative" }
            $Backup = Join-Path $Evidence ('previous/' + $Relative)
            $Existed = Test-Path -LiteralPath $Target
            if ($Existed) {
                [IO.Directory]::CreateDirectory((Split-Path $Backup -Parent)) | Out-Null
                [IO.File]::Replace($Temporary,$Target,$Backup)
            } else { [IO.File]::Move($Temporary,$Target) }
            $Changed.Add(@{target=$Target;backup=$Backup;existed=$Existed})
        } finally { if (Test-Path -LiteralPath $Temporary) { Remove-Item -LiteralPath $Temporary } }
    }
    foreach ($Entry in $Files) {
        if ((Get-FileHash -LiteralPath (Join-Path $Destination ([string]$Entry.path)) -Algorithm SHA256).Hash -ine $Entry.sha256) { throw "Installed hash mismatch: $($Entry.path)" }
    }
} catch {
    for ($Index = $Changed.Count - 1; $Index -ge 0; --$Index) {
        $Change = $Changed[$Index]
        if ($Change.existed) { [IO.File]::Replace($Change.backup,$Change.target,$null) }
        else { Remove-Item -LiteralPath $Change.target }
    }
    throw
}
# Retire only hash-matching files from the previous nested installation.
# Backup each file before removing it; modified and unlisted user files remain.
$Removed = [Collections.Generic.List[string]]::new()
if ($LegacyManifest) {
    $Parents = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($Entry in $LegacyManifest.files) {
        $Relative = [string]$Entry.path
        $OldFile = Join-Path $Legacy $Relative
        Assert-UpdatePlainPath $OldFile
        if (-not (Test-Path -LiteralPath $OldFile) -or (Get-FileHash -LiteralPath $OldFile -Algorithm SHA256).Hash -ine $Entry.sha256) { continue }
        $Backup = Join-Path $Evidence ('legacy/' + $Relative)
        [IO.Directory]::CreateDirectory((Split-Path $Backup -Parent)) | Out-Null
        Copy-Item -LiteralPath $OldFile -Destination $Backup
        if ((Get-FileHash -LiteralPath $Backup -Algorithm SHA256).Hash -ine $Entry.sha256) { throw "Legacy backup mismatch: $Relative" }
        Remove-Item -LiteralPath $OldFile
        $Removed.Add($Relative)
        $Parent = Split-Path $OldFile -Parent
        while ($Parent.StartsWith($Legacy + '\',[StringComparison]::OrdinalIgnoreCase)) { $null = $Parents.Add($Parent); $Parent = Split-Path $Parent -Parent }
    }
    $Remaining = @($LegacyManifest.files | Where-Object { Test-Path -LiteralPath (Join-Path $Legacy ([string]$_.path)) })
    if ($Remaining.Count -eq 0) {
        [IO.Directory]::CreateDirectory((Join-Path $Evidence 'legacy')) | Out-Null
        Copy-Item -LiteralPath $LegacyInventory -Destination (Join-Path $Evidence ('legacy/' + $ManifestName))
        Remove-Item -LiteralPath $LegacyInventory
    }
    $null = $Parents.Add($Legacy)
    foreach ($Folder in @($Parents | Sort-Object Length -Descending)) {
        Assert-UpdatePlainPath $Folder
        if ((Test-Path -LiteralPath $Folder) -and @(Get-ChildItem -LiteralPath $Folder -Force).Count -eq 0) { Remove-Item -LiteralPath $Folder }
    }
}
[IO.Directory]::CreateDirectory($Evidence) | Out-Null
Write-UpdateJson (Join-Path $Evidence 'installed.json') ([ordered]@{destination=$Destination;files=$Files;changed=$Changed.Count;removedLegacy=$Removed.ToArray();utc=[DateTime]::UtcNow.ToString('o')})
Write-Output "Installed HyperCompare: $Destination ($($Files.Count) verified files; receipt $Evidence)"

}
Export-ModuleMember -Function Test-HyperComparePackage,Install-HyperCompare
