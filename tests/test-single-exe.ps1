[CmdletBinding()]
param([Parameter(Mandatory)][string]$CompilerBinPath, [Parameter(Mandatory)][string]$SevenZipPath,
      [Parameter(Mandatory)][string]$SfxPath, [string]$StageRoot, [string]$PayloadDirectory)
. (Join-Path $PSScriptRoot '..\scripts\SingleExe.Common.ps1')
$stage = New-ReleaseTemp $StageRoot
Write-Host "SFX probe evidence: $stage"
$payload = Join-Path $stage 'payload'
if ($PayloadDirectory) {
    Assert-ReleaseLayout $PayloadDirectory
    Copy-ReleaseTree $PayloadDirectory $payload
    # Overwrite ONLY the task-owned copy with an inert program before making any runnable SFX.
    $realMain = Join-Path $payload 'Orange Tools.exe'
    [IO.File]::WriteAllBytes($realMain, [byte[]]@())
    # Exercise the full 64 MiB dictionary and multifile decoder, not just a tiny probe archive.
    $capacity = [IO.File]::Open((Join-Path $payload '_sfx_capacity_probe.bin'), [IO.FileMode]::CreateNew)
    try { $capacity.SetLength(65MB) } finally { $capacity.Dispose() }
} else { [IO.Directory]::CreateDirectory($payload) | Out-Null }
Invoke-ReleaseTool (Join-Path $CompilerBinPath 'g++.exe') @((Join-Path $PSScriptRoot 'fixtures\sfxprobe.cpp'),'-std=c++17','-Os','-s','-static','-o',(Join-Path $payload 'Orange Tools.exe')) (Join-Path $stage 'compile.log')
# Assert this executable is the static no-op probe (not the production GUI) before assembling SFX.
$probeHash = (Get-FileHash -LiteralPath (Join-Path $payload 'Orange Tools.exe')).Hash
if ($PayloadDirectory -and $probeHash -eq (Get-FileHash -LiteralPath (Join-Path $PayloadDirectory 'Orange Tools.exe')).Hash) { throw 'Probe substitution failed.' }
$archive = Join-Path $stage 'probe.7z'
Push-Location -LiteralPath $payload
try { Invoke-ReleaseTool $SevenZipPath @('a','-t7z',$archive,'.','-mx=9','-m0=LZMA','-mf=off','-ms=on','-md=64m','-mfb=273','-mmt=2','-mtc=off','-mta=off','-mtm=off','-spd','-bsp0') (Join-Path $stage 'pack.log') } finally { Pop-Location }
$runDirectory = Join-Path $stage '中文 空格 launch'
$tempDirectory = Join-Path $stage '中文 空格 temp'
[IO.Directory]::CreateDirectory($runDirectory) | Out-Null
[IO.Directory]::CreateDirectory($tempDirectory) | Out-Null
$sfx = Join-Path $runDirectory 'OrangeTools Probe.exe'
Join-Sfx $SfxPath $archive $sfx (Join-Path $stage 'config.txt')
function Start-Probe([string]$Exe, [string]$Marker) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Exe
    $info.Arguments = '-y'
    $info.WorkingDirectory = $runDirectory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.Environment['ORANGE_PROBE_MARKER'] = $Marker
    $info.Environment['TEMP'] = $tempDirectory
    $info.Environment['TMP'] = $tempDirectory
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $process = [Diagnostics.Process]::Start($info)
    return [pscustomobject]@{ Process=$process; Watch=$watch; Marker=$Marker }
}
function Wait-Probe($Run, [switch]$Corrupt) {
    if (-not $Run.Process.WaitForExit(20000)) { throw "SFX probe timed out; preserved process $($Run.Process.Id) and evidence $stage" }
    $Run.Watch.Stop()
    if ($Corrupt) {
        if ($Run.Process.ExitCode -eq 0 -or (Test-Path -LiteralPath $Run.Marker)) { throw 'Damaged SFX launched the probe or succeeded.' }
        return
    }
    if ($Run.Process.ExitCode -ne 0 -or $Run.Watch.ElapsedMilliseconds -lt 1700) { throw 'SFX did not successfully wait for the probe.' }
    if (-not (Test-Path -LiteralPath $Run.Marker)) { throw 'Probe was not launched.' }
    $lines = @(Get-Content -LiteralPath $Run.Marker -Encoding utf8)
    if ($lines.Count -ne 2 -or $lines[1] -ne (Join-Path $lines[0] 'Orange Tools.exe')) { throw 'SFX working directory differs from the extraction directory.' }
    if (-not $lines[0].StartsWith($tempDirectory + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'SFX did not use its independent system TEMP directory.' }
    if (Test-Path -LiteralPath $lines[0]) { throw 'SFX did not clean its released directory after exit.' }
    return $lines[0]
}
$single = Start-Probe $sfx (Join-Path $stage 'single.marker')
$singleDirectory = Wait-Probe $single
$first = Start-Probe $sfx (Join-Path $stage 'first.marker')
$second = Start-Probe $sfx (Join-Path $stage 'second.marker')
$firstDirectory = Wait-Probe $first
$secondDirectory = Wait-Probe $second
if ($firstDirectory -eq $secondDirectory) { throw 'Concurrent SFX instances share an extraction directory.' }
$damaged = Join-Path $runDirectory 'Damaged Probe.exe'
$bytes = [IO.File]::ReadAllBytes($sfx)
$bytes[$bytes.Length - 1] = $bytes[$bytes.Length - 1] -bxor 1
[IO.File]::WriteAllBytes($damaged, $bytes)
& $SevenZipPath t $damaged '-bsp0' *> (Join-Path $stage 'corruption.log')
if ($LASTEXITCODE -eq 0) { throw 'Damaged test fixture is not actually damaged.' }
$badRun = Start-Probe $damaged (Join-Path $stage 'damaged.marker')
Wait-Probe $badRun -Corrupt
if (@(Get-ChildItem -LiteralPath $tempDirectory -Force).Count -ne 0) { throw 'SFX left extraction-directory residue.' }
[pscustomobject]@{ ChineseAndSpaces=$true; CorrectWorkingDirectory=$true; WaitedForExit=$true; IsolatedConcurrentRuns=$true;
    CleanedExtraction=$true; RejectedCorruption=$true; FullPayloadDecoder=[bool]$PayloadDirectory; Stage=$stage } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage 'result.json') -Encoding utf8NoBOM
Write-Host 'SFX lifecycle probe passed (real main application was NOT started).'
