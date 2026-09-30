[CmdletBinding()]
param(
    [string]$ExecutablePath = (Join-Path $PSScriptRoot 'build\release-check\release\Orange Tools.exe'),
    [string]$QtBinPath,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'OrangeToolsApp'),
    [switch]$IncludeSoftwareOpenGL,
    [switch]$IncludeOptionalDependencies,
    [switch]$UseUpx,
    [string]$UpxPath
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

if ($UseUpx) {
    if ([string]::IsNullOrWhiteSpace($UpxPath)) {
        $upxCommand = Get-Command upx.exe -ErrorAction SilentlyContinue
        if (-not $upxCommand) { throw 'UPX not found. Pass -UpxPath before deploying with -UseUpx.' }
        $UpxPath = $upxCommand.Source
    }
    $UpxPath = (Resolve-Path -LiteralPath $UpxPath).Path
}

$output = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($output) | Out-Null
$target = Join-Path $output ([IO.Path]::GetFileName($executable))
if ($UseUpx -and [string]::Equals($executable, $target, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'UPX requires a separate output directory so the original Release executable stays intact.'
}
if (-not [string]::Equals($executable, $target, [StringComparison]::OrdinalIgnoreCase)) {
    Copy-Item -LiteralPath $executable -Destination $target -Force
}

# Plain Widgets currently does not use OpenGL; keep its optional software renderer opt-in.
$deployArguments = @('--release', '--force', '--compiler-runtime', '--no-translations', '--dir', $output)
if (-not $IncludeSoftwareOpenGL) {
    $deployArguments += '--no-opengl-sw'
}
if (-not $IncludeOptionalDependencies) {
    $deployArguments += @('--no-system-d3d-compiler', '--no-svg',
                          '--skip-plugin-types', 'generic,iconengines',
                          '--exclude-plugins', 'qgif,qico,qjpeg,qsvg,qcertonlybackend')
}
$deployArguments += $target

# Do not alter the user's PATH or copy DLLs into Windows system directories.
$originalPath = $env:PATH
try {
    $env:PATH = "$QtBinPath;$originalPath"
    & $deployTool @deployArguments
    if ($LASTEXITCODE -ne 0) {
        throw "windeployqt failed with exit code $LASTEXITCODE. Do not distribute the incomplete output."
    }
} finally {
    $env:PATH = $originalPath
}

# Resolve plugins relative to the executable instead of the developer's Qt installation.
[IO.File]::WriteAllText((Join-Path $output 'qt.conf'), "[Paths]`r`nPrefix = .`r`nPlugins = .`r`n", [Text.UTF8Encoding]::new($false))
$required = @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Network.dll',
              'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll',
              'platforms\qwindows.dll', 'styles\qmodernwindowsstyle.dll',
              'networkinformation\qnetworklistmanager.dll', 'tls\qschannelbackend.dll')
foreach ($file in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $output $file) -PathType Leaf)) {
        throw "Incomplete deployment: $file is missing."
    }
}
# windeployqt only copies files. Prune an explicit allowlist, never arbitrary DLLs.
$optionalFiles = @()
if (-not $IncludeSoftwareOpenGL) {
    $optionalFiles += 'opengl32sw.dll'
}
if (-not $IncludeOptionalDependencies) {
    $optionalFiles += @('D3Dcompiler_47.dll', 'Qt6Svg.dll', 'generic\qtuiotouchplugin.dll',
                        'iconengines\qsvgicon.dll', 'imageformats\qgif.dll', 'imageformats\qico.dll',
                        'imageformats\qjpeg.dll', 'imageformats\qsvg.dll', 'tls\qcertonlybackend.dll')
}
$outputPrefix = $output.TrimEnd('\') + '\'
foreach ($relativePath in $optionalFiles) {
    $optionalPath = [IO.Path]::GetFullPath((Join-Path $output $relativePath))
    if (-not $optionalPath.StartsWith($outputPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Optional file path outside deployment directory: $optionalPath"
    }
    if (-not (Test-Path -LiteralPath $optionalPath)) { continue }
    # Reject symlinks/junctions in the file or its parents before removing anything.
    $entryPath = $optionalPath
    while ($true) {
        $entry = Get-Item -LiteralPath $entryPath -Force
        if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Refusing to prune a reparse point: $entryPath"
        }
        if ([string]::Equals($entryPath, $output, [StringComparison]::OrdinalIgnoreCase)) { break }
        $entryPath = Split-Path -Parent $entryPath
    }
    if ((Get-Item -LiteralPath $optionalPath).PSIsContainer) {
        throw "Optional DLL path is not a regular file: $optionalPath"
    }
    Remove-Item -LiteralPath $optionalPath -ErrorAction Stop
    Write-Host "Removed unused optional dependency: $relativePath"
}

if ($UseUpx) {
    & (Join-Path $PSScriptRoot 'compress-release.ps1') -OutputDirectory $output -UpxPath $UpxPath `
        -ExecutableName ([IO.Path]::GetFileName($target))
}
Write-Host "Deployment complete: $target"
Write-Host 'Keep the entire output directory together. Do not distribute the executable by itself.'
