<# Full offline release. Default: four-candidate, verified single EXE; -Format 7z keeps the old format. #>
[CmdletBinding()]
param(
    [string]$QtBinPath,
    [string]$CompilerBinPath,
    [string]$SevenZipPath,
    [string]$UpxPath,
    [string]$SfxPath,
    [string]$QtLicensePath,
    [string]$QtDocsPath,
    [string]$OutputPath,
    [ValidateSet('SingleExe','7z')][string]$Format = 'SingleExe',
    [ValidateRange(1,32)][int]$Jobs = 4
)
. (Join-Path $PSScriptRoot 'scripts\SingleExe.Common.ps1')
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'Run this entry with PowerShell 7 (pwsh.exe).' }
if (-not $QtBinPath) { $QtBinPath = Split-Path -Parent (Get-Command qmake.exe -ErrorAction Stop).Source }
if (-not $CompilerBinPath) { $CompilerBinPath = Split-Path -Parent (Get-Command g++.exe -ErrorAction Stop).Source }
if (-not $SevenZipPath) { $SevenZipPath = (Get-Command 7z.exe -ErrorAction Stop).Source }
if (-not $SfxPath) { $SfxPath = Join-Path $PSScriptRoot 'third_party\7zip-sfx\7zS.sfx' }
$qtRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $QtBinPath))
if (-not $QtLicensePath) { $QtLicensePath = Join-Path $qtRoot 'Licenses' }
$version = & (Join-Path $QtBinPath 'qmake.exe') -query QT_VERSION
if ($LASTEXITCODE -ne 0) { throw 'Cannot query selected Qt kit.' }
if (-not $QtDocsPath) { $QtDocsPath = Join-Path $qtRoot ('Docs\Qt-'+$version) }
if (-not $OutputPath) {
    $name = if ($Format -eq 'SingleExe') { 'OrangeTools-Single.exe' } else { 'OrangeTools-Portable.7z' }
    $OutputPath = Join-Path $PSScriptRoot ('dist\'+$name)
}
$output = [IO.Path]::GetFullPath($OutputPath)
foreach ($file in @($output,$output+'.sha256')) {
    Assert-NoLinks $file
    if (Test-Path -LiteralPath $file) { throw "Refusing to overwrite delivery: $file" }
}
if ($Format -eq 'SingleExe') {
    [void](Assert-PinnedSfx $SfxPath (Join-Path $PSScriptRoot 'third_party\7zip-sfx\module.json'))
    if (-not $UpxPath) { $UpxPath = (Get-Command upx.exe -ErrorAction Stop).Source }
    if (-not (Test-Path -LiteralPath $UpxPath -PathType Leaf)) { throw "UPX not found: $UpxPath" }
}
foreach ($tool in @((Join-Path $QtBinPath 'qmake.exe'),(Join-Path $QtBinPath 'windeployqt.exe'),
                    (Join-Path $CompilerBinPath 'g++.exe'),(Join-Path $CompilerBinPath 'mingw32-make.exe'),$SevenZipPath)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw "Missing release tool: $tool" }
}
$stage = New-ReleaseTemp ''
Write-Host "Release evidence (preserved on failure/success): $stage"
$source = Join-Path $stage 'source'
[IO.Directory]::CreateDirectory($source) | Out-Null
# Snapshot only formal inputs. Never package the historical OrangeToolsApp directory.
foreach ($relative in @('src','tests','qiubai','third_party','OrangeTools.pro','resources.pri','resources.qrc','ouga.pri','ougacodecs.pri','app.rc','app.manifest','LICENSE')) {
    $inputPath = Join-Path $PSScriptRoot $relative
    Assert-NoLinks $inputPath
    Copy-Item -LiteralPath $inputPath -Destination $source -Recurse
}
$sourceManifest = @(Get-ReleaseManifest $source)
$sourceManifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage 'source-manifest.json') -Encoding utf8NoBOM
# Keep test paths short for legacy bundled offline tools; record the task-owned root.
$testArtifacts = New-ReleaseTemp (Join-Path ([IO.Path]::GetTempPath()) ('ot-tests-'+[guid]::NewGuid().ToString('N').Substring(0,8)))
$testArtifacts | Set-Content -LiteralPath (Join-Path $stage 'test-artifacts.txt') -Encoding utf8NoBOM
$oldPath = $env:PATH
$oldTemp = $env:TEMP
$oldTmp = $env:TMP
$oldPlatform = [Environment]::GetEnvironmentVariable('QT_QPA_PLATFORM','Process')
$buildTemp = Join-Path $stage 'build-temp'
[IO.Directory]::CreateDirectory($buildTemp) | Out-Null
function Invoke-QtRegression([string]$Executable, [string]$Build, [string[]]$ExtraArguments) {
    $names = @('LOCALAPPDATA','APPDATA','USERPROFILE','QT_QPA_PLATFORM','ORANGE_TEST_ARTIFACTS','ORANGE_TEST_LPMAKE','ORANGE_BUNDLED_TOOL_ROOT','ORANGE_DEPENDENCY_MOCK')
    $saved = @{}
    foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
    try {
        $isolatedHome = Join-Path $testArtifacts ([IO.Path]::GetFileNameWithoutExtension($Executable)+'-'+[guid]::NewGuid().ToString('N').Substring(0,8))
        [IO.Directory]::CreateDirectory($isolatedHome) | Out-Null
        foreach ($name in @('LOCALAPPDATA','APPDATA','USERPROFILE','ORANGE_TEST_ARTIFACTS')) { [Environment]::SetEnvironmentVariable($name,$isolatedHome,'Process') }
        $env:QT_QPA_PLATFORM = 'offscreen'
        $env:ORANGE_TEST_LPMAKE = Join-Path $source 'qiubai\bin\lpmake\lpmake.exe'
        $env:ORANGE_BUNDLED_TOOL_ROOT = Join-Path $source 'qiubai'
        [Environment]::SetEnvironmentVariable('ORANGE_DEPENDENCY_MOCK',$null,'Process')
        $report = Join-Path $Build 'qt-tests.txt'
        Invoke-ReleaseTool $Executable (@('-o',($report+',txt')) + $ExtraArguments) (Join-Path $Build 'tests.log')
        $result = Get-Content -LiteralPath $report -Raw
        if ($result -notmatch 'Totals:\s+(\d+) passed,\s+0 failed,\s+0 skipped,' -or [int]$Matches[1] -lt 3) { throw "Incomplete or failing Qt regression: $report" }
        Write-Host ($Executable.Split('\')[-1]+': '+$Matches[1]+' passed; report '+$report)
    } finally { foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') } }
}
function Build-ReleaseProject([string]$Project, [string]$Build, [string]$ResourceConfig) {
    [IO.Directory]::CreateDirectory($Build) | Out-Null
    Push-Location -LiteralPath $Build
    try {
        Invoke-ReleaseTool (Join-Path $QtBinPath 'qmake.exe') @($Project,'CONFIG-=debug','CONFIG+=release',('CONFIG+='+$ResourceConfig),('OUGA_CODEC_ROOT='+ (Join-Path (Split-Path -Parent $CompilerBinPath) 'opt'))) (Join-Path $Build 'qmake.log')
        $arguments = @('-j'+$Jobs)
        if (Test-Path -LiteralPath (Join-Path $Build 'Makefile.Release')) { $arguments += @('-f','Makefile.Release') }
        Invoke-ReleaseTool (Join-Path $CompilerBinPath 'mingw32-make.exe') $arguments (Join-Path $Build 'make.log')
    } finally { Pop-Location }
}
try {
    $env:PATH = "$CompilerBinPath;$QtBinPath;$(Join-Path (Split-Path -Parent $CompilerBinPath) 'opt\bin');$oldPath"
    $env:TEMP = $buildTemp
    $env:TMP = $buildTemp
    & (Join-Path $PSScriptRoot 'tests\test-release-scripts.ps1') -StageRoot (Join-Path $buildTemp 'script-safety')
    if ($Format -eq 'SingleExe') {
        & (Join-Path $PSScriptRoot 'tests\test-single-exe.ps1') -CompilerBinPath $CompilerBinPath -SevenZipPath $SevenZipPath -SfxPath $SfxPath -StageRoot (Join-Path $buildTemp 'sfx-probe')
    }
    $variants = if ($Format -eq 'SingleExe') { @('raw','zlib') } else { @('raw') }
    $builds = @{}
    foreach ($variant in $variants) {
        $config = if ($variant -eq 'raw') { 'uncompressed_resources' } else { 'compressed_resources' }
        Write-Host "Building $variant Release application, matching resources test and deployment probe..."
        $work = Join-Path $stage $variant
        $appBuild = Join-Path $work 'app'
        Build-ReleaseProject (Join-Path $source 'OrangeTools.pro') $appBuild $config
        $main = Join-Path $appBuild 'release\Orange Tools.exe'
        if (-not (Test-Path -LiteralPath $main)) { throw 'Main Release output missing.' }
        $testBuild = Join-Path $work 'readability'
        Build-ReleaseProject (Join-Path $source 'tests\readabilitytests.pro') $testBuild $config
        Invoke-QtRegression (Join-Path $testBuild 'release\ReadabilityTests.exe') $testBuild @('-platform','offscreen')
        $smokeBuild = Join-Path $work 'smoke'
        Build-ReleaseProject (Join-Path $source 'tests\deploymentsmoke.pro') $smokeBuild $config
        $smoke = Join-Path $smokeBuild 'release\DeploymentSmoke.exe'
        $deploy = Join-Path $work 'deployment'
        & (Join-Path $PSScriptRoot 'deploy.ps1') -ExecutablePath $main -QtBinPath $QtBinPath -OutputDirectory $deploy -CodecBinPath (Join-Path (Split-Path -Parent $CompilerBinPath) 'opt\bin') -PreserveExistingFiles *> (Join-Path $work 'deploy.log')
        Add-ReleaseLicenses $PSScriptRoot $deploy $QtLicensePath $QtDocsPath $CompilerBinPath
        Test-ReleaseDeployment $deploy $QtBinPath $CompilerBinPath $smoke (Join-Path $source 'qiubai\icon.png') (Join-Path $work 'check')
        $builds[$variant] = [pscustomobject]@{ Main=$main; Deployment=$deploy; Smoke=$smoke }
    }
    # Existing regressions use inert tools/fixtures or loopback servers, never real devices or external services.
    foreach ($spec in @(@('processmanagertests','ProcessManagerTests'),@('deviceoperationtests','DeviceOperationTests'),
                        @('ougadependencytests','OugaDependencyTests'),@('ougatests','OugaTests'),@('ouganetworktests','OugaNetworkTests'))) {
        Write-Host "Running regression: $($spec[1])"
        $build = Join-Path $stage ('regression\'+$spec[0])
        Build-ReleaseProject (Join-Path $source ('tests\'+$spec[0]+'.pro')) $build 'uncompressed_resources'
        # This suite asserts HWND minimize/taskbar behavior and native Windows layout metrics.
        $testArguments = if ($spec[1] -eq 'OugaTests') { @('-platform','windows') } else { @() }
        Invoke-QtRegression (Join-Path $build ('release\'+$spec[1]+'.exe')) $build $testArguments
    }
    $candidates = @()
    if ($Format -eq 'SingleExe') {
        foreach ($definition in @(@('A','raw',$false),@('B','zlib',$false),@('C','raw',$true),@('D','zlib',$true))) {
            $letter = $definition[0]; $variant = $definition[1]; $upx = $definition[2]
            Write-Host "Packaging and validating candidate $letter ($variant / UPX=$upx)..."
            $candidate = & (Join-Path $PSScriptRoot 'package-single-exe.ps1') -ReleaseDirectory $builds[$variant].Deployment -OutputPath (Join-Path $stage ($letter+'.exe')) -SevenZipPath $SevenZipPath -SfxPath $SfxPath -UpxPath $UpxPath -ResourceCompression $variant -UseUpx:$upx -StageRoot (Join-Path $buildTemp ('candidate-'+$letter))
            $candidate | Add-Member -NotePropertyName Candidate -NotePropertyValue $letter
            Test-ReleaseDeployment (Join-Path $candidate.Stage 'extracted') $QtBinPath $CompilerBinPath $builds[$variant].Smoke (Join-Path $source 'qiubai\icon.png') (Join-Path $candidate.Stage 'check')
            $candidates += $candidate
            $candidates | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'candidates.json') -Encoding utf8NoBOM
        }
        $winner = $candidates | Sort-Object Bytes,Upx,ResourceCompression | Select-Object -First 1
        & (Join-Path $PSScriptRoot 'tests\test-single-exe.ps1') -CompilerBinPath $CompilerBinPath -SevenZipPath $SevenZipPath -SfxPath $SfxPath -StageRoot (Join-Path $buildTemp 'sfx-full-payload-probe') -PayloadDirectory (Join-Path $winner.Stage 'payload')
        $selected = $builds[$winner.ResourceCompression]
    } else { $selected = $builds['raw'] }
    Assert-ReleaseManifest $sourceManifest $source
    # Update only this worktree; preserve unknown/user files and never touch the desktop checkout.
    $localRuntime = Join-Path $PSScriptRoot 'OrangeToolsApp'
    & (Join-Path $PSScriptRoot 'deploy.ps1') -ExecutablePath $selected.Main -QtBinPath $QtBinPath -OutputDirectory $localRuntime -CodecBinPath (Join-Path (Split-Path -Parent $CompilerBinPath) 'opt\bin') -PreserveExistingFiles *> (Join-Path $stage 'local-deploy.log')
    $runtimeSource = if ($Format -eq 'SingleExe') { Join-Path $winner.Stage 'payload' } else { $selected.Deployment }
    foreach ($file in Get-ReleaseManifest $runtimeSource) {
        $target = Join-Path $localRuntime $file.Path
        Assert-NoLinks $target
        [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
        Copy-Item -LiteralPath (Join-Path $runtimeSource $file.Path) -Destination $target -Force
        if ((Get-FileHash -LiteralPath $target).Hash -ne $file.Sha256) { throw "Local runtime update mismatch: $($file.Path)" }
    }
    # Validate only formal files in a replica, leaving any user data untouched in the runtime directory.
    $localCheck = Join-Path $stage 'local-runtime-verified'
    [IO.Directory]::CreateDirectory($localCheck) | Out-Null
    foreach ($file in Get-ReleaseManifest $runtimeSource) {
        $target = Join-Path $localCheck $file.Path
        [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
        Copy-Item -LiteralPath (Join-Path $localRuntime $file.Path) -Destination $target
    }
    Test-ReleaseDeployment $localCheck $QtBinPath $CompilerBinPath $selected.Smoke (Join-Path $source 'qiubai\icon.png') (Join-Path $stage 'local-check')
    if ($Format -eq 'SingleExe') {
        # Verify the selected complete EXE once more BEFORE creating deliverables.
        Invoke-ReleaseTool $SevenZipPath @('t',$winner.Output,'-bsp0') (Join-Path $stage 'final-test.log')
        $finalExtract = Join-Path $stage 'final-extracted'
        Invoke-ReleaseTool $SevenZipPath @('x',$winner.Output,('-o'+$finalExtract),'-y','-bsp0') (Join-Path $stage 'final-extract.log')
        Assert-ReleaseManifest @(Get-ReleaseManifest $runtimeSource) $finalExtract
        Write-NewReleaseFile $winner.Output $output
        $hash = Write-NewChecksum $output
        if ($hash -ne $winner.Sha256) { throw 'Delivery copy differs from the verified EXE.' }
        Write-Host "Winner $($winner.Candidate): $($winner.Bytes) bytes; SHA-256 $hash"
    } else {
        & (Join-Path $PSScriptRoot 'package-release.ps1') -ReleaseDirectory $selected.Deployment -ArchivePath $output -SevenZipPath $SevenZipPath
    }
    [pscustomobject]@{ Output=$output; Evidence=$stage; Format=$Format; Candidates=$candidates } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'result.json') -Encoding utf8NoBOM
    Write-Host "Verified release: $output"
} catch {
    $_ | Out-String | Set-Content -LiteralPath (Join-Path $stage 'failure.txt') -Encoding utf8NoBOM
    Write-Error "Release stopped. Evidence retained: $stage. Do not distribute partial outputs. $($_.Exception.Message)"
    throw
} finally { $env:PATH=$oldPath; $env:TEMP=$oldTemp; $env:TMP=$oldTmp; [Environment]::SetEnvironmentVariable('QT_QPA_PLATFORM',$oldPlatform,'Process') }
