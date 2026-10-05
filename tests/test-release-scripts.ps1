[CmdletBinding()]
param([string]$StageRoot)
. (Join-Path $PSScriptRoot '..\scripts\SingleExe.Common.ps1')
$stage = New-ReleaseTemp $StageRoot
$checks = 0
function Must-Fail([scriptblock]$Action, [string]$Label) {
    $failed = $false
    try { & $Action } catch { $failed = $true }
    if (-not $failed) { throw "Missing rejection: $Label" }
    $script:checks++
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
Must-Fail { New-ReleaseTemp (Join-Path $PSScriptRoot 'forbidden-temp') } 'TEMP outside system TEMP'
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
