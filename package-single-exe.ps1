<# Packages a fresh, validated deployment; release.ps1 is the full build entry. #>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ReleaseDirectory,
    [string]$OutputPath,
    [string]$SevenZipPath,
    [string]$SfxPath,
    [string]$IconPath,
    [string]$UpxPath,
    [ValidateSet('raw','zlib')][string]$ResourceCompression = 'raw',
    [switch]$UseUpx,
    [string]$StageRoot
)
. (Join-Path $PSScriptRoot 'scripts\SingleExe.Common.ps1')
. (Join-Path $PSScriptRoot 'scripts\SfxIcon.ps1')
if (-not $IconPath) { $IconPath = Join-Path $PSScriptRoot 'assets\sfx.ico' }
if (-not $OutputPath) { $OutputPath = Join-Path $PSScriptRoot 'dist\OrangeTools-Single.exe' }
if (-not $SevenZipPath) { $SevenZipPath = (Get-Command 7z.exe -ErrorAction Stop).Source }
if (-not $SfxPath) { $SfxPath = Join-Path $PSScriptRoot 'third_party\7zip-sfx\7zS.sfx' }
$output = [IO.Path]::GetFullPath($OutputPath)
foreach ($file in @($output, $output + '.sha256')) {
    Assert-NoLinks $file
    if (Test-Path -LiteralPath $file) { throw "Refusing to overwrite: $file" }
}
$pin = Assert-PinnedSfx $SfxPath (Join-Path $PSScriptRoot 'third_party\7zip-sfx\module.json')
$release = (Resolve-Path -LiteralPath $ReleaseDirectory).Path
Assert-ReleaseLayout $release
if ([IO.Path]::GetExtension($output) -ne '.exe') { throw 'Single EXE output must end in .exe.' }
if ($output.StartsWith($release.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Output must be outside the release input.' }
$before = @(Get-ReleaseManifest $release)
$stage = New-ReleaseTemp $StageRoot
Write-Host "Packaging evidence: $stage"
$payload = Join-Path $stage 'payload'
Copy-ReleaseTree $release $payload
if ($UseUpx) {
    if (-not $UpxPath) { $UpxPath = (Get-Command upx.exe -ErrorAction Stop).Source }
    foreach ($name in @('Orange Tools.exe','libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')) {
        $target = Join-Path $payload $name
        $signature = Get-AuthenticodeSignature -LiteralPath $target
        if ($signature.Status -ne 'NotSigned') { throw "Cannot UPX-pack a signed or unverifiable file: $name" }
        $packed = Join-Path $stage ($name + '.upx')
        Invoke-ReleaseTool $UpxPath @('--best','--lzma','--strip-relocs=0','--compress-exports=0','--compress-icons=0','--compress-resources=0','-o',$packed,$target) (Join-Path $stage ($name+'.pack.log'))
        Invoke-ReleaseTool $UpxPath @('-t',$packed) (Join-Path $stage ($name+'.test.log'))
        $unpacked = Join-Path $stage ($name + '.unpacked')
        Invoke-ReleaseTool $UpxPath @('-d','-o',$unpacked,$packed) (Join-Path $stage ($name+'.unpack.log'))
        # UPX reconstructs PE headers/import tables; validate the original executable content sections.
        $originalSections = Get-PePayloadFingerprint $target
        $restoredSections = Get-PePayloadFingerprint $unpacked
        foreach ($section in $originalSections.Keys) {
            if (-not $restoredSections.ContainsKey($section) -or $restoredSections[$section] -ne $originalSections[$section]) { throw "UPX executable-content mismatch: $name / $section" }
        }
        [pscustomobject]@{ File=$name; OriginalSha256=(Get-FileHash -LiteralPath $target).Hash;
            RestoredSha256=(Get-FileHash -LiteralPath $unpacked).Hash; VerifiedContentSections=$originalSections } |
            ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $stage ($name+'.roundtrip.json')) -Encoding utf8NoBOM
        Copy-Item -LiteralPath $packed -Destination $target
    }
}
Assert-ReleaseLayout $payload
$manifest = @(Get-ReleaseManifest $payload)
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $stage 'manifest.json') -Encoding utf8NoBOM
$archive = Join-Path $stage 'payload.7z'
Push-Location -LiteralPath $payload
try {
    # '.' includes the directory contents even with -spd; literal '*' produces an empty archive.
    Invoke-ReleaseTool $SevenZipPath @('a','-t7z',$archive,'.','-mx=9','-m0=LZMA','-mf=off','-ms=on','-md=64m','-mfb=273','-mmt=2','-mtc=off','-mta=off','-mtm=off','-spd','-bsp0') (Join-Path $stage 'archive.log')
} finally { Pop-Location }
Invoke-ReleaseTool $SevenZipPath @('t',$archive,'-bsp0') (Join-Path $stage 'archive-test.log')
$combined = Join-Path $stage 'OrangeTools-Single.exe'
$iconModule = New-IconSfxModule $SfxPath $IconPath (Join-Path $stage 'icon-7zS.sfx')
Join-Sfx $iconModule $archive $combined (Join-Path $stage 'config.txt')
Invoke-ReleaseTool $SevenZipPath @('t',$combined,'-bsp0') (Join-Path $stage 'sfx-test.log')
$extracted = Join-Path $stage 'extracted'
Invoke-ReleaseTool $SevenZipPath @('x',$combined,('-o'+$extracted),'-y','-bsp0') (Join-Path $stage 'extract.log')
[OrangeSfxIcon]::Verify($combined, $IconPath)
Assert-ReleaseManifest $manifest $extracted
Assert-ReleaseManifest $before $release
Write-NewReleaseFile $combined $output
$hash = Write-NewChecksum $output
[pscustomobject]@{ Output=$output; Bytes=(Get-Item -LiteralPath $output).Length; UnpackedBytes=($manifest | Measure-Object Bytes -Sum).Sum;
    Sha256=$hash; Files=$manifest.Count; ResourceCompression=$ResourceCompression; Upx=$UseUpx.IsPresent; SfxVersion=$pin.version; Stage=$stage }
