[CmdletBinding()]
param([string]$StageRoot)
. (Join-Path $PSScriptRoot '..\scripts\Protection.Common.ps1')
$stage = New-ReleaseTemp $StageRoot
$checks = 0
function Must-Fail([scriptblock]$Action, [string]$Label) {
    $failed = $false
    try { & $Action } catch { $failed = $true }
    if (-not $failed) { throw "Missing rejection: $Label" }
    $script:checks++
}
$payload = Join-Path $stage '中文 空格 payload'
[IO.Directory]::CreateDirectory($payload) | Out-Null
foreach ($name in @('LICENSE.txt','qt.conf','runtime.bat','licenses\required-source.cpp')) {
    $path = Join-Path $payload $name
    [IO.Directory]::CreateDirectory((Split-Path -Parent $path)) | Out-Null
    [IO.File]::WriteAllText($path,'inert fixture')
}
Assert-ReleasePayload $payload
$checks++
foreach ($name in @('own.cpp','header.h','OrangeTools.pro','local.pri','object.o','symbols.pdb','OrangeTools.debug','build.log','Makefile.Release','licenses\bad.debug')) {
    $path = Join-Path $payload $name
    [IO.File]::WriteAllText($path,'inert fixture')
    Must-Fail { Assert-ReleasePayload -Root $payload -Files @($path) } $name
}
# An unchanged user file is reported, not removed; auditing only newly generated files succeeds.
Assert-ReleasePayload -Root $payload -Files @((Join-Path $payload 'qt.conf'))
if (-not (Test-Path -LiteralPath (Join-Path $payload 'own.cpp'))) { throw 'Audit deleted a file' }
$checks++
Must-Fail { Assert-ReleasePayload -Root $payload -Files @((Join-Path $stage 'outside.cpp')) } 'outside audit root'
Must-Fail { New-ReleaseTemp (Join-Path $PSScriptRoot 'outside-temp') } 'outside project TEMP'
Must-Fail { New-ReleaseTemp $stage } 'TEMP reuse'
$asciiHome = Join-Path $stage 'ascii-test-home'
[IO.Directory]::CreateDirectory($asciiHome) | Out-Null
$drive = New-ReleaseAsciiTestHome $asciiHome
try {
    if (-not [IO.Directory]::Exists($drive.Home)) { throw 'ASCII TEMP alias is inaccessible' }
    $checks++
} finally { Remove-ReleaseAsciiTestHome $drive }
if ([OrangeProtection.TestDrive]::Target($drive.Drive)) { throw 'ASCII TEMP alias leaked' }
$checks++
# Minimal inert PE header, never executed. Inject sensitive text in both encodings.
$exe = Join-Path $stage 'Orange Tools.exe'
$bytes = [byte[]]::new(256)
$bytes[0]=0x4d; $bytes[1]=0x5a; $bytes[0x3c]=64; $bytes[64]=0x50; $bytes[65]=0x45
[IO.File]::WriteAllBytes($exe,$bytes)
$source = Split-Path -Parent $PSScriptRoot
$inventory = @(Get-ProtectionInventory $source)
if ($inventory.Count -ne 6) { throw 'Sensitive inventory is incomplete or format changed' }
[void](Assert-ProtectedBinary $exe $source)
$checks++
foreach ($encoding in @([Text.Encoding]::UTF8,[Text.Encoding]::Unicode)) {
    $text = $encoding.GetBytes($inventory[0].Text)
    [IO.File]::WriteAllBytes($exe,($bytes + $text))
    Must-Fail { Assert-ProtectedBinary $exe $source } ('plaintext '+$encoding.WebName)
}
# GNU debuglink may use a COFF string table without exposing symbols.
$bytes[76]=128
[IO.File]::WriteAllBytes($exe,$bytes)
[void](Assert-ProtectedBinary $exe $source)
$checks++
$bytes[80]=1
[IO.File]::WriteAllBytes($exe,$bytes)
Must-Fail { Assert-ProtectedBinary $exe $source } 'COFF symbols'
$bytes[80]=0
foreach ($encoding in @([Text.Encoding]::UTF8,[Text.Encoding]::Unicode)) {
    [IO.File]::WriteAllBytes($exe,($bytes + $encoding.GetBytes($source)))
    Must-Fail { Assert-ProtectedBinary $exe $source } ('source path '+$encoding.WebName)
}
[IO.File]::WriteAllBytes($exe,$bytes)
$manifest = Join-Path $stage 'protection.json'
@{ Executable='Orange Tools.exe'; Sha256=(Get-FileHash -LiteralPath $exe).Hash } | ConvertTo-Json |
    Set-Content -LiteralPath $manifest -Encoding utf8NoBOM
Assert-ProtectionManifest $exe $manifest
$checks++
$bytes[200]=1
[IO.File]::WriteAllBytes($exe,$bytes)
Must-Fail { Assert-ProtectionManifest $exe $manifest } 'changed protected EXE'
Write-Host "$checks protection safety checks passed; $stage"
@{ Passed=$checks } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage 'result.json') -Encoding utf8NoBOM
