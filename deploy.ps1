[CmdletBinding()]
param(
    [string]$ExecutablePath = (Join-Path $PSScriptRoot 'build\release-check\release\Orange Tools.exe'),
    [string]$QtBinPath,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'OrangeToolsApp'),
    [switch]$IncludeSoftwareOpenGL,
    [switch]$IncludeOptionalDependencies,
    [switch]$PreserveExistingFiles,
    [switch]$UseUpx,
    [string]$UpxPath,
    # Folder with libbz2-1.dll from the MinGW kit that linked the executable
    # (mingw64\opt\bin). Defaults to the g++ found on PATH.
    [string]$CodecBinPath,
    [string]$ProtectionManifestPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'scripts\Protection.Common.ps1')

if (-not (Test-Path -LiteralPath $ExecutablePath -PathType Leaf)) {
    throw "Release executable not found: $ExecutablePath. Build first or pass -ExecutablePath."
}
$executable = (Resolve-Path -LiteralPath $ExecutablePath).Path
if ($ProtectionManifestPath) { Assert-ProtectionManifest $executable $ProtectionManifestPath }
$buildMakefile = Join-Path (Split-Path -Parent (Split-Path -Parent $executable)) 'Makefile.Release'
if ((Test-Path -LiteralPath $buildMakefile) -and
    [IO.File]::ReadAllText($buildMakefile).Contains('ORANGE_PROTECTED_RELEASE') -and -not $ProtectionManifestPath) {
    throw 'Protected Release requires the manifest from Finalize-ProtectedExecutable before deployment.'
}
# The statically linked Zstandard decoder's BSD notice must accompany binaries.
$zstdLicense = Join-Path $PSScriptRoot 'third_party\zstd\LICENSE'
if (-not (Test-Path -LiteralPath $zstdLicense -PathType Leaf)) {
    throw "Zstandard license is missing: $zstdLicense"
}

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
# Validate before any writes. Do not traverse links or partially update a running app.
$ancestor = $output
while (-not [string]::IsNullOrWhiteSpace($ancestor)) {
    if (Test-Path -LiteralPath $ancestor) {
        $item = Get-Item -LiteralPath $ancestor -Force
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Deployment path contains a symlink/junction: $ancestor"
        }
    }
    $ancestor = Split-Path -Parent $ancestor
}
if (Test-Path -LiteralPath $output) {
    if (-not (Get-Item -LiteralPath $output).PSIsContainer) { throw 'Deployment output is not a directory.' }
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($output)
    while ($pending.Count -gt 0) {
        foreach ($item in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Deployment contains a symlink/junction: $($item.FullName)"
            }
            if ($item.PSIsContainer) { $pending.Push($item.FullName); continue }
            if ($item.Extension -in @('.exe', '.dll') -or $item.Name -eq 'qt.conf') {
                try {
                    $handle = [IO.File]::Open($item.FullName, [IO.FileMode]::Open,
                                              [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
                    $handle.Dispose()
                } catch {
                    throw "Deployment file is locked or not writable; close the application and retry: $($item.FullName)"
                }
            }
        }
    }
}
$before = @{}
if (Test-Path -LiteralPath $output) {
    foreach ($file in Get-ChildItem -LiteralPath $output -Recurse -File -Force) {
        $before[$file.FullName] = (Get-FileHash -LiteralPath $file.FullName).Hash
    }
}
[IO.Directory]::CreateDirectory($output) | Out-Null
$target = Join-Path $output ([IO.Path]::GetFileName($executable))
if ($UseUpx -and [string]::Equals($executable, $target, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'UPX requires a separate output directory so the original Release executable stays intact.'
}
if (-not [string]::Equals($executable, $target, [StringComparison]::OrdinalIgnoreCase)) {
    Copy-Item -LiteralPath $executable -Destination $target -Force
}

# Plain Widgets currently does not use OpenGL; keep its optional software renderer opt-in.
$deployArguments = @('--release', '--force', '--compiler-runtime', '--concurrent', '--no-translations',
                     '--qtpaths', (Join-Path $QtBinPath 'qtpaths.exe'), '--dir', $output)
if (-not $IncludeSoftwareOpenGL) {
    $deployArguments += '--no-opengl-sw'
}
if (-not $IncludeOptionalDependencies) {
    $deployArguments += @('--no-system-d3d-compiler',
                          '--skip-plugin-types', 'generic,iconengines',
                          '--exclude-plugins', 'qgif,qico,qjpeg,qsvg,qcertonlybackend')
}
# Inspect the build artifact, not the output beside potentially stale Qt DLLs.
$deployArguments += $executable

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

# In-process Payload extraction links bzip2 dynamically; windeployqt does not copy it.
if ([string]::IsNullOrWhiteSpace($CodecBinPath)) {
    $compiler = Get-Command g++.exe -ErrorAction SilentlyContinue
    if ($compiler) { $CodecBinPath = Join-Path (Split-Path -Parent (Split-Path -Parent $compiler.Source)) 'opt\bin' }
}
$codec = if ([string]::IsNullOrWhiteSpace($CodecBinPath)) { $null } else { Join-Path $CodecBinPath 'libbz2-1.dll' }
if (-not $codec -or -not (Test-Path -LiteralPath $codec -PathType Leaf)) {
    throw 'libbz2-1.dll not found. Pass -CodecBinPath pointing to the MinGW kit opt\bin used for the build.'
}
Copy-Item -LiteralPath $codec -Destination (Join-Path $output 'libbz2-1.dll') -Force
$licenseDirectory = Join-Path $output 'licenses'
[IO.Directory]::CreateDirectory($licenseDirectory) | Out-Null
Copy-Item -LiteralPath $zstdLicense -Destination (Join-Path $licenseDirectory 'zstd-BSD.txt') -Force

# Resolve plugins relative to the executable instead of the developer's Qt installation.
[IO.File]::WriteAllText((Join-Path $output 'qt.conf'), "[Paths]`r`nPrefix = .`r`nPlugins = .`r`n", [Text.UTF8Encoding]::new($false))
$required = @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Network.dll', 'Qt6Concurrent.dll', 'Qt6Svg.dll',
              'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll', 'libbz2-1.dll',
              'licenses\zstd-BSD.txt', 'platforms\qwindows.dll', 'styles\qmodernwindowsstyle.dll',
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
    $optionalFiles += @('D3Dcompiler_47.dll', 'generic\qtuiotouchplugin.dll',
                        'iconengines\qsvgicon.dll', 'imageformats\qgif.dll', 'imageformats\qico.dll',
                        'imageformats\qjpeg.dll', 'imageformats\qsvg.dll', 'tls\qcertonlybackend.dll')
}
# Safe in-place application updates must not prune any existing files.
if ($PreserveExistingFiles) { $optionalFiles = @() }
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
$generated = @(Get-ChildItem -LiteralPath $output -Recurse -File -Force | Where-Object {
    -not $before.ContainsKey($_.FullName) -or $before[$_.FullName] -ne (Get-FileHash -LiteralPath $_.FullName).Hash
} | ForEach-Object FullName)
Assert-ReleasePayload -Root $output -Files $generated
foreach ($relative in @(Get-ReleaseLeaks $output)) {
    Write-Warning "Existing source/debug/build residue retained (not newly deployed): $relative"
}
if (-not $UseUpx -and (Get-FileHash -LiteralPath $target).Hash -ne (Get-FileHash -LiteralPath $executable).Hash) {
    throw 'Deployed executable hash differs from the current build; output may be incomplete.'
}
Write-Host "Deployment complete: $target"
Write-Host 'Keep the entire output directory together. Do not distribute the executable by itself.'
