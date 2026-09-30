[CmdletBinding()]
param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\OrangeToolsApp'),
    [string]$SmokeExecutable = (Join-Path $PSScriptRoot '..\build\deployment-smoke\release\DeploymentSmoke.exe'),
    [string]$ImagePath = (Join-Path $PSScriptRoot '..\qiubai\icon.png')
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$output = (Resolve-Path -LiteralPath $OutputDirectory).Path.TrimEnd('\')
$source = (Resolve-Path -LiteralPath $SmokeExecutable).Path
$image = (Resolve-Path -LiteralPath $ImagePath).Path
$target = [IO.Path]::GetFullPath((Join-Path $output '_deployment_smoke.exe'))
if (-not $target.StartsWith($output + '\', [StringComparison]::OrdinalIgnoreCase) -or
    ((Get-Item -LiteralPath $output).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw 'Invalid smoke test target directory.'
}
if (Test-Path -LiteralPath $target) { throw "Refusing to overwrite existing smoke test: $target" }
$names = @('PATH', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QT_QPA_PLATFORM',
           'QT_STYLE_OVERRIDE', 'QML_IMPORT_PATH', 'QML2_IMPORT_PATH')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    Copy-Item -LiteralPath $source -Destination $target
    $env:PATH = "$output;$([Environment]::SystemDirectory);$env:SystemRoot"
    foreach ($name in $names | Where-Object { $_ -ne 'PATH' }) {
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }
    & $target $image 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Deployment smoke failed with exit code $LASTEXITCODE" }
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        Remove-Item -LiteralPath $target -ErrorAction Stop
    }
}
