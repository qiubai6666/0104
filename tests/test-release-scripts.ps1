[CmdletBinding()]
param([string]$StageRoot)
. (Join-Path $PSScriptRoot '..\scripts\SingleExe.Common.ps1')
$stage = New-ReleaseTemp $StageRoot
$checks = 0
. (Join-Path $PSScriptRoot '..\scripts\ReleaseInputs.ps1')
function Must-Fail([scriptblock]$Action, [string]$Label) {
    $failed = $false
    try { & $Action } catch { $failed = $true }
    if (-not $failed) { throw "Missing rejection: $Label" }
    $script:checks++
}
# An inert project fixture checks shared input parity without invoking qmake.
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$projectFixture = Join-Path $stage 'source-fixture'
[IO.Directory]::CreateDirectory($projectFixture) | Out-Null
foreach ($name in Get-ReleaseInputPaths $projectRoot) {
    $from = Join-Path $projectRoot $name
    $to = Join-Path $projectFixture $name
    if (Test-Path -LiteralPath $from -PathType Container) {
        [IO.Directory]::CreateDirectory($to) | Out-Null
    } else {
        Copy-Item -LiteralPath $from -Destination $to -ErrorAction Stop
    }
}
$snapshot = Join-Path $stage 'source-snapshot'
Copy-ReleaseSourceSnapshot $projectFixture $snapshot ${function:Assert-NoLinks}
foreach ($name in @('openlist.pri','xiaomi.pri','ouga.pri','ougacodecs.pri','resources.pri','protected-release.pri')) {
    if (-not (Test-Path -LiteralPath (Join-Path $snapshot $name) -PathType Leaf)) { throw "Missing snapshot include: $name" }
    $checks++
}
if (-not (Test-Path -LiteralPath (Join-Path $snapshot 'assets') -PathType Container)) { throw 'Missing snapshot assets directory' }
$checks++
foreach ($entry in @('release.ps1','build-protected-release.ps1')) {
    $text = Get-Content -LiteralPath (Join-Path $projectRoot $entry) -Raw
    if ($text -notmatch 'Copy-ReleaseSourceSnapshot' -or $text -notmatch 'ReleaseInputs.ps1') { throw "Entry does not share release inputs: $entry" }
    $checks++
}
foreach ($name in @('openlist.pri','resources.pri','ougacodecs.pri','app.rc','assets')) {
    $path = [IO.Path]::GetFullPath((Join-Path $projectFixture $name))
    if (-not $path.StartsWith($projectFixture + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw "Fixture path escaped: $path" }
    Move-Item -LiteralPath $path -Destination ($path + '.missing') -ErrorAction Stop
    try { Must-Fail { Copy-ReleaseSourceSnapshot $projectFixture (Join-Path $stage ('missing-' + $name)) ${function:Assert-NoLinks} } "missing required source $name" }
    finally { Move-Item -LiteralPath ($path + '.missing') -Destination $path -ErrorAction Stop }
}
$inputRoot = Join-Path $stage '中文 空格 input'
[IO.Directory]::CreateDirectory($inputRoot) | Out-Null
foreach ($name in Get-RequiredReleaseFiles) {
    $target = Join-Path $inputRoot $name
    [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
    [IO.File]::WriteAllText($target,'inert fixture')
}
foreach ($name in @('licenses\OrangeTools-MIT.txt','licenses\zstd-BSD.txt','licenses\7zip-sfx\License.txt',
    'licenses\7zip-sfx\COPYING.LGPL-2.1.txt','licenses\Qt\LICENSE.txt','licenses\MinGW\bzip2\LICENSE.txt')) {
    $target = Join-Path $inputRoot $name
    [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
    [IO.File]::WriteAllText($target,'inert fixture')
}
Assert-ReleaseLayout $inputRoot
$manifest = @(Get-ReleaseManifest $inputRoot)
$replica = Join-Path $stage 'replica'
Copy-ReleaseTree $inputRoot $replica
Assert-ReleaseManifest $manifest $replica
$checks++
[IO.File]::WriteAllText((Join-Path $replica 'Orange Tools.exe'),'modified fixture')
Must-Fail { Assert-ReleaseManifest $manifest $replica } 'changed member'
[IO.File]::WriteAllText((Join-Path $inputRoot '_deployment_smoke.exe'),'inert test')
Must-Fail { Assert-ReleaseLayout $inputRoot } 'test program in payload'
Must-Fail { New-ReleaseTemp $stage } 'existing experiment directory'
Must-Fail { New-ReleaseTemp (Join-Path $PSScriptRoot 'forbidden-temp') } 'TEMP outside project TEMP'
$module = Join-Path $PSScriptRoot '..\third_party\7zip-sfx\7zS.sfx'
$pin = Join-Path $PSScriptRoot '..\third_party\7zip-sfx\module.json'
[void](Assert-PinnedSfx $module $pin)
$checks++
$damaged = Join-Path $stage 'changed.sfx'
$bytes = [IO.File]::ReadAllBytes($module)
$bytes[$bytes.Length-1] = $bytes[$bytes.Length-1] -bxor 1
[IO.File]::WriteAllBytes($damaged,$bytes)
Must-Fail { Assert-PinnedSfx $damaged $pin } 'changed module'
$protected = Join-Path $stage 'protected.exe'
[IO.File]::WriteAllText($protected,'keep existing delivery')
$before = (Get-FileHash -LiteralPath $protected).Hash
Must-Fail { Write-NewReleaseFile $damaged $protected } 'delivery overwrite'
if ((Get-FileHash -LiteralPath $protected).Hash -ne $before) { throw 'Existing delivery was modified.' }
$checks++
[pscustomobject]@{ Passed=$checks; Stage=$stage } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage 'result.json') -Encoding utf8NoBOM
Write-Host "$checks release-script safety checks passed. Evidence: $stage"
