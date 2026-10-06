# Build and validate an offline protected Release, update OrangeToolsApp, never package.
[CmdletBinding()]
param(
    [string]$QtBinPath,
    [string]$CompilerBinPath,
    [string]$QtLicensePath,
    [string]$QtDocsPath,
    [string]$StageRoot,
    [ValidateRange(1,32)][int]$Jobs = 4
)
. (Join-Path $PSScriptRoot 'scripts\Protection.Build.ps1')
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'Use PowerShell 7 (pwsh.exe)' }
if (-not $QtBinPath) { $QtBinPath = Split-Path -Parent (Get-Command qmake.exe -ErrorAction Stop).Source }
if (-not $CompilerBinPath) { $CompilerBinPath = Split-Path -Parent (Get-Command g++.exe -ErrorAction Stop).Source }
$QtBinPath = (Resolve-Path -LiteralPath $QtBinPath).Path
$CompilerBinPath = (Resolve-Path -LiteralPath $CompilerBinPath).Path
$qtRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $QtBinPath))
if (-not $QtLicensePath) { $QtLicensePath = Join-Path $qtRoot 'Licenses' }
$version = & (Join-Path $QtBinPath 'qmake.exe') -query QT_VERSION
if ($LASTEXITCODE) { throw 'Cannot query selected Qt kit' }
if (-not $QtDocsPath) { $QtDocsPath = Join-Path $qtRoot ('Docs\Qt-'+$version.Trim()) }
$stage = New-ReleaseTemp $StageRoot
Write-Host "Protection evidence/private symbols (do not distribute): $stage"
$source = Join-Path $stage 'source'
[IO.Directory]::CreateDirectory($source) | Out-Null
foreach ($relative in @('src','tests','qiubai','third_party','OrangeTools.pro','protected-release.pri','resources.pri','resources.qrc',
                        'ouga.pri','xiaomi.pri','ougacodecs.pri','app.rc','app.manifest','LICENSE')) {
    Assert-NoLinks (Join-Path $PSScriptRoot $relative)
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $relative) -Destination $source -Recurse
}
$sourceManifest = @(Get-ReleaseManifest $source)
$sourceManifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage 'source-manifest.json') -Encoding utf8NoBOM
$saved = @{}
foreach ($name in @('PATH','TEMP','TMP')) { $saved[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
try {
    $env:PATH = "$CompilerBinPath;$QtBinPath;$(Join-Path (Split-Path -Parent $CompilerBinPath) 'opt\bin');$env:PATH"
    $env:TEMP = $stage; $env:TMP = $stage
    & (Join-Path $PSScriptRoot 'tests\test-protection-scripts.ps1') -StageRoot (Join-Path $stage 'script-tests')
    $results = @()
    $deployments = @{}
    foreach ($protected in @($false,$true)) {
        $label = if ($protected) { 'protected' } else { 'normal' }
        $work = Join-Path $stage $label
        $app = Join-Path $work 'app'
        Build-ProtectionProject (Join-Path $source 'OrangeTools.pro') $app $QtBinPath $CompilerBinPath $protected $Jobs
        $exe = Join-Path $app 'release\Orange Tools.exe'
        $protection = @{}
        if ($protected) {
            $protection.ProtectionManifestPath = Finalize-ProtectedExecutable $exe $app $CompilerBinPath (Join-Path $work 'private-symbols') $source
        }
        foreach ($spec in @(@('encodedstringtests','EncodedStringTests'),@('readabilitytests','ReadabilityTests'))) {
            $build = Join-Path $work $spec[0]
            Build-ProtectionProject (Join-Path $source ('tests\'+$spec[0]+'.pro')) $build $QtBinPath $CompilerBinPath $protected $Jobs
            Invoke-ProtectionRegression (Join-Path $build ('release\'+$spec[1]+'.exe')) $build $source @()
        }
        $smokeBuild = Join-Path $work 'smoke'
        Build-ProtectionProject (Join-Path $source 'tests\deploymentsmoke.pro') $smokeBuild $QtBinPath $CompilerBinPath $protected $Jobs
        $smoke = Join-Path $smokeBuild 'release\DeploymentSmoke.exe'
        $runtime = Join-Path $work 'deployment'
        & (Join-Path $PSScriptRoot 'deploy.ps1') -ExecutablePath $exe -QtBinPath $QtBinPath -OutputDirectory $runtime `
            -CodecBinPath (Join-Path (Split-Path -Parent $CompilerBinPath) 'opt\bin') -PreserveExistingFiles @protection *> (Join-Path $work 'deploy.log')
        Add-ReleaseLicenses $source $runtime $QtLicensePath $QtDocsPath $CompilerBinPath
        Assert-ReleasePayload $runtime
        Test-ReleaseDeployment $runtime $QtBinPath $CompilerBinPath $smoke (Join-Path $source 'qiubai\icon.png') (Join-Path $work 'check')
        foreach ($spec in @(@('processmanagertests','ProcessManagerTests'),@('deviceoperationtests','DeviceOperationTests'),
            @('deviceinformationtests','DeviceInformationTests'),@('screencasttests','ScreenCastTests'),
            @('ougadependencytests','OugaDependencyTests'),@('ougatests','OugaTests'),@('ouganetworktests','OugaNetworkTests'),@('xiaomitests','XiaomiTests'))) {
            $build = Join-Path $work ('regressions\'+$spec[0])
            Build-ProtectionProject (Join-Path $source ('tests\'+$spec[0]+'.pro')) $build $QtBinPath $CompilerBinPath $protected $Jobs
            $arguments = if ($spec[1] -eq 'OugaTests') { @('-platform','windows') } else { @() }
            Invoke-ProtectionRegression (Join-Path $build ('release\'+$spec[1]+'.exe')) $build $source $arguments
        }
        $deployments[$label] = @{ Exe=$exe; Runtime=$runtime; Smoke=$smoke; Protection=$protection }
        $results += [pscustomobject]@{ Configuration=$label; Bytes=(Get-Item -LiteralPath $exe).Length;
            Build=(Get-Content -LiteralPath (Join-Path $app 'build-metrics.json') -Raw | ConvertFrom-Json);
            StartupChainTests=(Get-Content -LiteralPath (Join-Path $work 'readabilitytests\test-metrics.json') -Raw | ConvertFrom-Json) }
    }
    Assert-ReleaseManifest $sourceManifest $source
    $results | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $stage 'comparison.json') -Encoding utf8NoBOM
    $selected = $deployments['protected']
    $proof = $selected.Protection
    $output = Join-Path $PSScriptRoot 'OrangeToolsApp'
    & (Join-Path $PSScriptRoot 'deploy.ps1') -ExecutablePath $selected.Exe -QtBinPath $QtBinPath -OutputDirectory $output `
        -CodecBinPath (Join-Path (Split-Path -Parent $CompilerBinPath) 'opt\bin') -PreserveExistingFiles @proof *> (Join-Path $stage 'local-deploy.log')
    # Copy only verified formal members, preserving all unknown/config/data files.
    foreach ($file in Get-ReleaseManifest $selected.Runtime) {
        $target = Join-Path $output $file.Path
        Assert-NoLinks $target
        [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
        Copy-Item -LiteralPath (Join-Path $selected.Runtime $file.Path) -Destination $target -Force
        if ((Get-FileHash -LiteralPath $target).Hash -ne $file.Sha256) { throw "Local deployment differs: $($file.Path)" }
    }
    $replica = Join-Path $stage 'local-verified'
    [IO.Directory]::CreateDirectory($replica) | Out-Null
    foreach ($file in Get-ReleaseManifest $selected.Runtime) {
        $target = Join-Path $replica $file.Path
        [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
        Copy-Item -LiteralPath (Join-Path $output $file.Path) -Destination $target
    }
    Test-ReleaseDeployment $replica $QtBinPath $CompilerBinPath $selected.Smoke (Join-Path $source 'qiubai\icon.png') (Join-Path $stage 'local-check')
    Write-Host "Verified protected runtime: $output; private symbols/evidence: $stage"
} catch {
    $_ | Out-String | Set-Content -LiteralPath (Join-Path $stage 'failure.txt') -Encoding utf8NoBOM
    throw "Protected release stopped; do not distribute partial output. Evidence: $stage. $($_.Exception.Message)"
} finally { foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') } }
