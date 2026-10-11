Set-StrictMode -Version Latest

function Get-ReleaseInputPaths {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$ProjectRoot)

    $root = [IO.Path]::GetFullPath($ProjectRoot)
    $paths = [System.Collections.Generic.List[string]]::new()
    foreach ($relative in @('src', 'tests', 'assets', 'qiubai', 'third_party')) {
        $paths.Add($relative)
    }

    # Keep every root-level qmake include in the isolated source snapshot.
    foreach ($file in Get-ChildItem -LiteralPath $root -File -Filter '*.pri' | Sort-Object Name) {
        $paths.Add($file.Name)
    }
    foreach ($relative in @('OrangeTools.pro', 'resources.qrc', 'app.rc', 'app.manifest', 'LICENSE')) {
        $paths.Add($relative)
    }
    return $paths.ToArray()
}

function Copy-ReleaseSourceSnapshot {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ProjectRoot,
        [Parameter(Mandatory)][string]$Destination,
        [Parameter(Mandatory)][scriptblock]$AssertPath
    )

    $root = [IO.Path]::GetFullPath($ProjectRoot)
    $target = [IO.Path]::GetFullPath($Destination)
    & $AssertPath $target
    $inputs = @(Get-ReleaseInputPaths $root)
    foreach ($relative in $inputs) {
        $source = Join-Path $root $relative
        if (-not (Test-Path -LiteralPath $source)) {
            throw "Missing required release input: $relative"
        }
        & $AssertPath $source
    }

    # Validate includes in the main project and all root-level .pri files before
    # creating a snapshot. Both include(resources.pri) and $$PWD/foo.pri count.
    foreach ($relative in $inputs | Where-Object { $_ -match '\.(pro|pri)$' }) {
        $text = Get-Content -LiteralPath (Join-Path $root $relative) -Raw
        foreach ($match in [regex]::Matches($text, '(?im)^\s*include\s*\(\s*(?:\$\$PWD/)?([^/\\()]+\.pri)\s*\)')) {
            $included = $match.Groups[1].Value.Trim()
            if ($included -notin $inputs -or -not (Test-Path -LiteralPath (Join-Path $root $included) -PathType Leaf)) {
                throw "Missing root-level qmake release input: $included (referenced by $relative)"
            }
        }
    }

    [IO.Directory]::CreateDirectory($target) | Out-Null
    foreach ($relative in $inputs) {
        Copy-Item -LiteralPath (Join-Path $root $relative) -Destination $target -Recurse -Force -ErrorAction Stop
        if (-not (Test-Path -LiteralPath (Join-Path $target $relative))) {
            throw "Release input was not copied: $relative"
        }
    }
}
