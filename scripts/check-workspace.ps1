[CmdletBinding()]
param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [switch]$Strict
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path.TrimEnd('\')
if (-not (Test-Path -LiteralPath (Join-Path $ProjectRoot 'OrangeTools.pro') -PathType Leaf)) {
    throw "Not the OrangeTools project root: $ProjectRoot"
}

$allowedRootFiles = @(
    '.gitattributes', '.gitignore', 'AGENTS.md', 'LICENSE', 'OrangeTools.pro', 'README.md',
    'app.manifest', 'app.rc', 'build-protected-release.ps1', 'compress-release.ps1',
    'deploy.ps1', 'openlist.pri', 'ouga.pri', 'ougacodecs.pri', 'package-release.ps1',
    'package-single-exe.ps1', 'protected-release.pri', 'release.ps1', 'resources.pri',
    'resources.qrc', 'xiaomi.pri'
)
$allowedRootDirs = @(
    '.git', 'assets', 'build', 'dist', 'docs', 'OrangeToolsApp', 'qiubai',
    'scripts', 'src', 'TEMP', 'tests', 'third_party'
)
$requiredFiles = @(
    'OrangeTools.pro', 'resources.pri', 'resources.qrc', 'app.rc', 'app.manifest',
    'openlist.pri', 'ouga.pri', 'ougacodecs.pri', 'protected-release.pri', 'xiaomi.pri',
    'README.md', 'AGENTS.md', 'LICENSE', '.gitignore', '.gitattributes'
)
$requiredDirs = @('src', 'assets', 'tests', 'scripts', 'docs', 'third_party', 'qiubai')
$allowedUntrackedRoots = @(
    'src', 'assets', 'tests', 'scripts', 'docs', 'third_party', 'qiubai',
    'build', 'dist', 'OrangeToolsApp', 'TEMP'
)

$errors = [System.Collections.Generic.List[string]]::new()
$warnings = [System.Collections.Generic.List[string]]::new()

$rootEntries = @(Get-ChildItem -LiteralPath $ProjectRoot -Force)
foreach ($entry in $rootEntries) {
    if ($entry.Name -eq '.git') { continue }
    if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        $errors.Add("Root entry is a reparse point: $($entry.Name)")
        continue
    }
    if ($entry.PSIsContainer) {
        if ($entry.Name -notin $allowedRootDirs) {
            $errors.Add("Unexpected root directory: $($entry.Name)")
        }
    } elseif ($entry.Name -notin $allowedRootFiles) {
        $hint = if ($entry.Name -match '\.(exe|dll|o|obj|pdb|log|zip|7z|sha256)$' -or
            $entry.Name -like 'Makefile*' -or $entry.Name -in @('.qmake.stash', 'qt.conf')) {
            ' (build/deployment artifact)'
        } else { '' }
        $errors.Add("Unexpected root file: $($entry.Name)$hint")
    }
}

foreach ($name in $requiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $ProjectRoot $name) -PathType Leaf)) {
        $errors.Add("Missing required file: $name")
    }
}
foreach ($name in $requiredDirs) {
    if (-not (Test-Path -LiteralPath (Join-Path $ProjectRoot $name) -PathType Container)) {
        $errors.Add("Missing required directory: $name")
    }
}

$qrcCount = 0
try {
    [xml]$qrc = Get-Content -LiteralPath (Join-Path $ProjectRoot 'resources.qrc') -Raw
    $qrcFiles = @($qrc.SelectNodes('/RCC/qresource/file'))
    $qrcCount = $qrcFiles.Count
    foreach ($node in $qrcFiles) {
        $relative = $node.InnerText
        if ([string]::IsNullOrWhiteSpace($relative)) { continue }
        if (-not (Test-Path -LiteralPath (Join-Path $ProjectRoot $relative) -PathType Leaf)) {
            $errors.Add("resources.qrc references missing file: $relative")
        }
    }
} catch {
    $errors.Add("Cannot parse resources.qrc: $($_.Exception.Message)")
}

try {
    $rcText = Get-Content -LiteralPath (Join-Path $ProjectRoot 'app.rc') -Raw
    foreach ($match in [regex]::Matches($rcText, '(?im)\bICON\s+"([^"]+)"')) {
        $relative = $match.Groups[1].Value
        if (-not (Test-Path -LiteralPath (Join-Path $ProjectRoot $relative) -PathType Leaf)) {
            $errors.Add("app.rc references missing icon: $relative")
        }
    }
} catch {
    $errors.Add("Cannot parse app.rc: $($_.Exception.Message)")
}

$dist = Join-Path $ProjectRoot 'dist'
if (Test-Path -LiteralPath $dist -PathType Container) {
    foreach ($file in @(Get-ChildItem -LiteralPath $dist -File | Where-Object { $_.Extension -in @('.exe', '.7z') })) {
        $checksum = $file.FullName + '.sha256'
        if (-not (Test-Path -LiteralPath $checksum -PathType Leaf)) {
            $errors.Add("dist deliverable has no checksum: $($file.Name)")
            continue
        }
        $expected = ((Get-Content -LiteralPath $checksum -Raw).Trim() -split '\s+')[0]
        $actual = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        if ($expected -ne $actual) {
            $errors.Add("dist checksum mismatch: $($file.Name)")
        }
    }
    foreach ($dir in @(Get-ChildItem -LiteralPath $dist -Directory)) {
        $warnings.Add("dist contains unpacked directory: $($dir.Name)")
    }
    foreach ($file in @(Get-ChildItem -LiteralPath $dist -File |
        Where-Object { $_.Extension -notin @('.exe', '.7z', '.sha256') })) {
        $warnings.Add("dist contains non-delivery file: $($file.Name)")
    }
}

$build = Join-Path $ProjectRoot 'build'
if (Test-Path -LiteralPath $build -PathType Container) {
    foreach ($dir in @(Get-ChildItem -LiteralPath $build -Directory | Where-Object { $_.Name -ne 'release-check' })) {
        $warnings.Add("build contains extra task directory: $($dir.Name)")
    }
    foreach ($file in @(Get-ChildItem -LiteralPath $build -File)) {
        $warnings.Add("build root contains loose file: $($file.Name)")
    }
    $releaseCheckExe = Join-Path $build 'release-check\release\Orange Tools.exe'
    if (-not (Test-Path -LiteralPath $releaseCheckExe -PathType Leaf)) {
        $warnings.Add('build/release-check output is missing')
    }
}

$temp = Join-Path $ProjectRoot 'TEMP'
if (Test-Path -LiteralPath $temp -PathType Container) {
    $taskDirs = @(Get-ChildItem -LiteralPath $temp -Directory)
    if ($taskDirs.Count -gt 1) {
        $warnings.Add("TEMP contains $($taskDirs.Count) task directories; keep at most 1")
    }
    foreach ($file in @(Get-ChildItem -LiteralPath $temp -File)) {
        $warnings.Add("TEMP root contains loose file: $($file.Name)")
    }
}

$app = Join-Path $ProjectRoot 'OrangeToolsApp'
if (Test-Path -LiteralPath $app -PathType Container) {
    if (-not (Test-Path -LiteralPath (Join-Path $app 'Orange Tools.exe') -PathType Leaf)) {
        $warnings.Add('OrangeToolsApp is missing Orange Tools.exe')
    }
}

try {
    $gitOutput = @(& git -C $ProjectRoot -c core.quotepath=false ls-files --others --exclude-standard 2>$null)
    $gitExit = $LASTEXITCODE
    if ($gitExit -ne 0) {
        $warnings.Add("git ls-files failed with exit code $gitExit")
    } else {
        foreach ($relative in $gitOutput) {
            if ([string]::IsNullOrWhiteSpace($relative)) { continue }
            $normalized = $relative -replace '\\', '/'
            $first = ($normalized -split '/', 2)[0]
            if ($first -notin $allowedUntrackedRoots) {
                $errors.Add("Untracked path outside allowed directories: $normalized")
            }
        }
    }
} catch {
    $warnings.Add("Cannot inspect untracked files: $($_.Exception.Message)")
}

foreach ($warning in $warnings) {
    Write-Host "WARN  $warning" -ForegroundColor Yellow
}
if ($Strict -and $warnings.Count -gt 0) {
    foreach ($warning in $warnings) { $errors.Add("Strict: $warning") }
}
foreach ($errorItem in $errors) {
    Write-Host "ERROR $errorItem" -ForegroundColor Red
}

$fileCount = @($rootEntries | Where-Object { -not $_.PSIsContainer }).Count
$dirCount = @($rootEntries | Where-Object { $_.PSIsContainer }).Count
Write-Host ("Checked root: {0} files, {1} directories; qrc entries: {2}" -f $fileCount, $dirCount, $qrcCount)
if ($errors.Count -gt 0) {
    Write-Host "Workspace check FAILED: $($errors.Count) error(s), $($warnings.Count) warning(s)." -ForegroundColor Red
    exit 1
}
Write-Host "Workspace check OK: 0 errors, $($warnings.Count) warning(s)." -ForegroundColor Green
exit 0
