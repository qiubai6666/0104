# Shared helpers. No application/device/network actions and no experiment cleanup.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
function Assert-NoLinks([string]$Path) {
    $absolute = [IO.Path]::GetFullPath($Path)
    $ancestor = $absolute
    while ($ancestor) {
        if (Test-Path -LiteralPath $ancestor) {
            if ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Refusing a linked path: $ancestor" }
        }
        $ancestor = Split-Path -Parent $ancestor
    }
    if (Test-Path -LiteralPath $absolute -PathType Container) {
        $pending = [Collections.Generic.Stack[string]]::new()
        $pending.Push($absolute)
        while ($pending.Count) {
            foreach ($item in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
                if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Refusing a linked entry: $($item.FullName)" }
                if ($item.PSIsContainer) { $pending.Push($item.FullName) }
            }
        }
    }
}
function New-ReleaseTemp([string]$Path) {
    if (-not $Path) { $Path = Join-Path ([IO.Path]::GetTempPath()) ('orangetools-single-' + [guid]::NewGuid().ToString('N')) }
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Experimental work must stay inside system TEMP.' }
    Assert-NoLinks $full
    if (Test-Path -LiteralPath $full) { throw "Refusing to reuse existing TEMP directory: $full" }
    [IO.Directory]::CreateDirectory($full) | Out-Null
    return $full
}
function Invoke-ReleaseTool([string]$Tool, [string[]]$Arguments, [string]$Log) {
    & $Tool @Arguments *> $Log
    if ($LASTEXITCODE -ne 0) {
        Get-Content -LiteralPath $Log -Tail 30 | Write-Host
        throw "Tool failed ($LASTEXITCODE): $Tool; evidence: $Log"
    }
}
function Get-ReleaseManifest([string]$Root) {
    Assert-NoLinks $Root
    $base = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')
    return @(Get-ChildItem -LiteralPath $base -Recurse -File -Force | Sort-Object FullName | ForEach-Object {
        [pscustomobject]@{ Path=$_.FullName.Substring($base.Length + 1); Bytes=$_.Length; Sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
}
function Assert-ReleaseManifest([object[]]$Expected, [string]$Root) {
    $actual = @(Get-ReleaseManifest $Root)
    if ($actual.Count -ne $Expected.Count) { throw "File count mismatch: $Root" }
    $map = @{}
    foreach ($file in $actual) { $map[$file.Path] = $file }
    foreach ($file in $Expected) {
        if (-not $map.ContainsKey($file.Path) -or $map[$file.Path].Bytes -ne $file.Bytes -or $map[$file.Path].Sha256 -ne $file.Sha256) { throw "File content mismatch: $($file.Path) in $Root" }
    }
}
function Copy-ReleaseTree([string]$Source, [string]$Destination) {
    Assert-NoLinks $Source
    Assert-NoLinks $Destination
    [IO.Directory]::CreateDirectory($Destination) | Out-Null
    foreach ($item in Get-ChildItem -LiteralPath $Source -Force) { Copy-Item -LiteralPath $item.FullName -Destination $Destination -Recurse -Force }
}
function Get-RequiredReleaseFiles {
    return @('Orange Tools.exe','qt.conf','Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll','Qt6Network.dll','Qt6Concurrent.dll','Qt6Svg.dll',
        'libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll','libbz2-1.dll',
        'platforms\qwindows.dll','styles\qmodernwindowsstyle.dll','networkinformation\qnetworklistmanager.dll','tls\qschannelbackend.dll')
}
function Assert-ReleaseLayout([string]$Root) {
    $manifest = @(Get-ReleaseManifest $Root)
    $allowed = @(Get-RequiredReleaseFiles)
    foreach ($relative in $allowed) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $relative) -PathType Leaf)) { throw "Missing runtime dependency: $relative" }
    }
    foreach ($file in $manifest) {
        if ($file.Path -notin $allowed -and $file.Path -notmatch '^licenses\\[\w .\\-]+\.(txt|md|json)$') { throw "Unapproved release member (history/test/build residue): $($file.Path)" }
    }
    foreach ($relative in @('licenses\OrangeTools-MIT.txt','licenses\zstd-BSD.txt','licenses\7zip-sfx\License.txt',
                            'licenses\7zip-sfx\COPYING.LGPL-2.1.txt','licenses\Qt\LICENSE.txt','licenses\MinGW\bzip2\LICENSE.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $relative) -PathType Leaf)) { throw "Missing license: $relative" }
    }
}
function Get-SfxConfiguration {
    # Quote an absolute extraction-directory path; never search beside the downloaded EXE.
    return @'
;!@Install@!UTF-8!
Title="Orange Tools"
Progress="no"
Directory=""
RunProgram="\"%%T\\Orange Tools.exe\""
;!@InstallEnd@!
'@ + [Environment]::NewLine
}
function Join-Sfx([string]$Module, [string]$Archive, [string]$Output, [string]$ConfigPath) {
    [IO.File]::WriteAllText($ConfigPath, (Get-SfxConfiguration), [Text.UTF8Encoding]::new($false))
    $outputStream = [IO.File]::Open($Output, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        foreach ($part in @($Module,$ConfigPath,$Archive)) {
            $inputStream = [IO.File]::OpenRead($part)
            try { $inputStream.CopyTo($outputStream) } finally { $inputStream.Dispose() }
        }
    } finally { $outputStream.Dispose() }
}
function Assert-PinnedSfx([string]$Path, [string]$ManifestPath) {
    $pin = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $pin.sha256 -or (Get-Item -LiteralPath $Path).Length -ne $pin.bytes) { throw 'SFX does not match the pinned official module; do not distribute it.' }
    return $pin
}
function Write-NewReleaseFile([string]$Source, [string]$Output) {
    Assert-NoLinks $Output
    [IO.Directory]::CreateDirectory((Split-Path -Parent $Output)) | Out-Null
    $dest = [IO.File]::Open($Output, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $inputStream = [IO.File]::OpenRead($Source)
    try { $inputStream.CopyTo($dest) } finally { $inputStream.Dispose(); $dest.Dispose() }
}
function Write-NewChecksum([string]$Output) {
    $hash = (Get-FileHash -LiteralPath $Output -Algorithm SHA256).Hash
    $line = $hash + ' *' + [IO.Path]::GetFileName($Output) + [Environment]::NewLine
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($line)
    $stream = [IO.File]::Open($Output + '.sha256', [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
    return $hash
}

function Add-ReleaseLicenses([string]$Project, [string]$Deployment, [string]$QtLicensePath, [string]$QtDocsPath, [string]$CompilerBinPath) {
    $directory = Join-Path $Deployment 'licenses'
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    Copy-Item -LiteralPath (Join-Path $Project 'LICENSE') -Destination (Join-Path $directory 'OrangeTools-MIT.txt')
    # Runtime resource extraction excludes documentation; ship tool licenses separately.
    $toolLicenses = @{
        'qiubai\bin\7zip\License.txt' = '7zip\License.txt'
        'qiubai\bin\lpmake\LICENSE-AOSP.txt' = 'lpmake\LICENSE-AOSP.txt'
    }
    foreach ($source in $toolLicenses.Keys) {
        $destination = Join-Path $directory $toolLicenses[$source]
        [IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
        Copy-Item -LiteralPath (Join-Path $Project $source) -Destination $destination -ErrorAction Stop
    }
    $sfxDirectory = Join-Path $directory '7zip-sfx'
    [IO.Directory]::CreateDirectory($sfxDirectory) | Out-Null
    foreach ($name in @('License.txt','COPYING.LGPL-2.1.txt','upstream-readme.txt','module.json')) {
        Copy-Item -LiteralPath (Join-Path $Project ('third_party\7zip-sfx\'+$name)) -Destination $sfxDirectory
    }
    $qtDirectory = Join-Path $directory 'Qt'
    [IO.Directory]::CreateDirectory($qtDirectory) | Out-Null
    foreach ($name in @('LICENSE','Copyright.txt','COPYING.txt')) {
        $destination = if ($name -eq 'LICENSE') { 'LICENSE.txt' } else { $name }
        Copy-Item -LiteralPath (Join-Path $QtLicensePath $name) -Destination (Join-Path $qtDirectory $destination)
    }
    # Preserve attribution and complete embedded license texts from the matching offline Qt docs.
    foreach ($module in @('qtcore','qtgui','qtwidgets','qtnetwork','qtconcurrent','qtsvg')) {
        $docs = @(Get-ChildItem -LiteralPath (Join-Path $QtDocsPath $module) -Filter '*attribution*.html' -File)
        if ($module -in @('qtcore','qtgui','qtsvg') -and $docs.Count -eq 0) { throw "Missing offline Qt license attribution pages: $module" }
        foreach ($doc in $docs) {
            $html = [IO.File]::ReadAllText($doc.FullName)
            $html = [regex]::Replace($html, '(?is)<(script|style|head)\b[^>]*>.*?</\1>', '')
            $text = [Net.WebUtility]::HtmlDecode([regex]::Replace($html, '(?s)<[^>]+>', ' '))
            $text = [regex]::Replace($text, '[ \t]+', ' ')
            [IO.File]::WriteAllText((Join-Path $qtDirectory ($doc.BaseName+'.txt')), $text, [Text.UTF8Encoding]::new($false))
        }
    }
    $mingwLicenses = Join-Path (Split-Path -Parent $CompilerBinPath) 'licenses'
    foreach ($module in @('gcc','mingw-w64','winpthreads','bzip2','xz','zlib')) {
        $target = Join-Path $directory ('MinGW\'+$module)
        [IO.Directory]::CreateDirectory($target) | Out-Null
        foreach ($file in Get-ChildItem -LiteralPath (Join-Path $mingwLicenses $module) -File) {
            Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $target ($file.Name+'.txt'))
        }
    }
    Copy-Item -LiteralPath (Join-Path $mingwLicenses 'bzip2\COPYING') -Destination (Join-Path $directory 'MinGW\bzip2\LICENSE.txt')
}
function Test-ReleaseDeployment([string]$Deployment, [string]$QtBinPath, [string]$CompilerBinPath,
                               [string]$SmokeExecutable, [string]$ImagePath, [string]$CheckRoot) {
    Assert-ReleaseLayout $Deployment
    [IO.Directory]::CreateDirectory($CheckRoot) | Out-Null
    $signatureRecords = @()
    foreach ($relative in @(Get-RequiredReleaseFiles) | Where-Object { $_ -match '^(Qt6|platforms\\|styles\\|networkinformation\\|tls\\)' }) {
        $file = Join-Path $Deployment $relative
        $kitSource = if ($relative.StartsWith('Qt6')) { Join-Path $QtBinPath $relative } else { Join-Path (Split-Path -Parent $QtBinPath) ('plugins\'+$relative) }
        $signature = Get-AuthenticodeSignature -LiteralPath $file
        if ($signature.Status -ne 'Valid' -or -not $signature.SignerCertificate -or $signature.SignerCertificate.Subject -notmatch 'The Qt Company') { throw "Qt signature failed: $relative ($($signature.Status))" }
        if ((Get-FileHash -LiteralPath $file).Hash -ne (Get-FileHash -LiteralPath $kitSource).Hash) { throw "Qt file differs from selected Release kit: $relative" }
        $signatureRecords += [pscustomobject]@{ Path=$relative; Status=[string]$signature.Status; Signer=$signature.SignerCertificate.Subject; Sha256=(Get-FileHash -LiteralPath $file).Hash }
    }
    $signatureRecords | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $CheckRoot 'signatures.json') -Encoding utf8NoBOM
    # Check every PE import, including transitive DLLs; only Windows system/API-set DLLs may be external.
    foreach ($file in Get-ChildItem -LiteralPath $Deployment -Recurse -File | Where-Object Extension -in @('.dll','.exe')) {
        $log = Join-Path $CheckRoot ($file.Name+'.imports.log')
        Invoke-ReleaseTool (Join-Path $CompilerBinPath 'objdump.exe') @('-p',$file.FullName) $log
        foreach ($line in Get-Content -LiteralPath $log) {
            if ($line -match 'DLL Name:\s+(\S+)') {
                $name = $Matches[1]
                if ($name -match '^(api-ms-|ext-ms-)') { continue }
                if (-not (Test-Path -LiteralPath (Join-Path $Deployment $name)) -and -not (Test-Path -LiteralPath (Join-Path ([Environment]::SystemDirectory) $name))) { throw "Missing PE dependency: $name required by $($file.Name)" }
            }
        }
    }
    # The smoke EXE is only copied into an isolated validation replica, never into the input/package.
    $run = Join-Path $CheckRoot 'runtime'
    Copy-ReleaseTree $Deployment $run
    $probe = Join-Path $run '_deployment_smoke.exe'
    Copy-Item -LiteralPath $SmokeExecutable -Destination $probe
    $names = @('PATH','QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','QT_QPA_PLATFORM','QT_STYLE_OVERRIDE','QML_IMPORT_PATH','QML2_IMPORT_PATH')
    $saved = @{}
    foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
    try {
        $env:PATH = "$run;$([Environment]::SystemDirectory);$env:SystemRoot"
        foreach ($name in $names | Where-Object { $_ -ne 'PATH' }) { [Environment]::SetEnvironmentVariable($name,$null,'Process') }
        Invoke-ReleaseTool $probe @($ImagePath) (Join-Path $CheckRoot 'smoke.log')
    } finally { foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') } }
}

function Get-PePayloadFingerprint([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) {
        throw "Not a PE executable: $Path"
    }
    $pe = [BitConverter]::ToUInt32($bytes, 0x3c)
    if ($pe + 24 -gt $bytes.Length -or [BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550) {
        throw "Invalid PE header: $Path"
    }
    $count = [BitConverter]::ToUInt16($bytes, $pe + 6)
    $table = $pe + 24 + [BitConverter]::ToUInt16($bytes, $pe + 20)
    $fingerprints = @{}
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        for ($index = 0; $index -lt $count; $index++) {
            $entry = $table + $index * 40
            if ($entry + 40 -gt $bytes.Length) { throw "Invalid PE section table: $Path" }
            $name = [Text.Encoding]::ASCII.GetString($bytes, $entry, 8).TrimEnd([char]0)
            if ($name -notin @('.text', '.data', '.rdata', '.pdata', '.xdata', '.CRT', '.tls', '.rsrc', '.reloc', '.edata')) {
                continue
            }
            $size = [BitConverter]::ToUInt32($bytes, $entry + 16)
            $offset = [BitConverter]::ToUInt32($bytes, $entry + 20)
            if ([long]$offset + $size -gt $bytes.Length) { throw "Invalid PE section: $Path / $name" }
            $fingerprints[$name] = [BitConverter]::ToString($hasher.ComputeHash($bytes, $offset, $size))
        }
    } finally { $hasher.Dispose() }
    if (-not $fingerprints.ContainsKey('.text') -or -not $fingerprints.ContainsKey('.rdata')) {
        throw "Missing executable code/resource sections: $Path"
    }
    return $fingerprints
}
