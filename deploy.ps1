[CmdletBinding()]
param(
    [string]$ExecutablePath = (Join-Path $PSScriptRoot 'build\release-check\release\FloatingWindow.exe'),
    [string]$QtBinPath,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'FloatingWindowApp')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $ExecutablePath -PathType Leaf)) {
    throw "Release executable not found: $ExecutablePath. Build first or pass -ExecutablePath."
}
$executable = (Resolve-Path -LiteralPath $ExecutablePath).Path

# Prefer the Qt kit recorded by the executable's own qmake build.
if ([string]::IsNullOrWhiteSpace($QtBinPath)) {
    $exeDirectory = Split-Path -Parent $executable
    $makefiles = @(
        (Join-Path $exeDirectory 'Makefile'),
        (Join-Path (Split-Path -Parent $exeDirectory) 'Makefile')
    )
    foreach ($makefile in $makefiles) {
        if (Test-Path -LiteralPath $makefile -PathType Leaf) {
            $match = [regex]::Match([IO.File]::ReadAllText($makefile), '(?m)^QMAKE\s*=\s*(.+?)\r?$')
            if ($match.Success) {
                $qmake = $match.Groups[1].Value.Trim().Trim('"')
                if (Test-Path -LiteralPath $qmake -PathType Leaf) {
                    $QtBinPath = Split-Path -Parent $qmake
                    break
                }
            }
        }
    }
    if ([string]::IsNullOrWhiteSpace($QtBinPath)) {
        $command = Get-Command windeployqt.exe -ErrorAction SilentlyContinue
        if ($command) { $QtBinPath = Split-Path -Parent $command.Source }
    }
}
if ([string]::IsNullOrWhiteSpace($QtBinPath)) {
    throw 'Qt kit not found. Pass -QtBinPath pointing to the bin directory of the Qt kit used to build this executable.'
}
$deployTool = Join-Path $QtBinPath 'windeployqt.exe'
if (-not (Test-Path -LiteralPath $deployTool -PathType Leaf)) {
    throw "Deployment tool not found: $deployTool"
}
$QtBinPath = (Resolve-Path -LiteralPath $QtBinPath).Path

$output = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($output) | Out-Null
$target = Join-Path $output ([IO.Path]::GetFileName($executable))
if (-not [string]::Equals($executable, $target, [StringComparison]::OrdinalIgnoreCase)) {
    Copy-Item -LiteralPath $executable -Destination $target -Force
}

# Do not alter the user's PATH or copy DLLs into Windows system directories.
$originalPath = $env:PATH
try {
    $env:PATH = "$QtBinPath;$originalPath"
    & $deployTool --release --compiler-runtime --no-translations --dir $output $target
    if ($LASTEXITCODE -ne 0) {
        throw "windeployqt failed with exit code $LASTEXITCODE. Do not distribute the incomplete output."
    }
} finally {
    $env:PATH = $originalPath
}

# Resolve plugins relative to the executable instead of the developer's Qt installation.
[IO.File]::WriteAllText((Join-Path $output 'qt.conf'), "[Paths]`r`nPrefix = .`r`nPlugins = .`r`n", [Text.UTF8Encoding]::new($false))
$required = @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Network.dll', 'platforms\qwindows.dll')
foreach ($file in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $output $file) -PathType Leaf)) {
        throw "Incomplete deployment: $file is missing."
    }
}
Write-Host "Deployment complete: $target"
Write-Host 'Keep the entire output directory together. Do not distribute the executable by itself.'
