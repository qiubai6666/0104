[CmdletBinding()]
param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'OrangeToolsApp'),
    [string]$ExecutableName = 'Orange Tools.exe',
    [string]$UpxPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($UpxPath)) {
    $command = Get-Command upx.exe -ErrorAction SilentlyContinue
    if (-not $command) { throw 'UPX not found. Pass -UpxPath pointing to an official UPX executable.' }
    $UpxPath = $command.Source
}
$upx = (Resolve-Path -LiteralPath $UpxPath).Path
$output = (Resolve-Path -LiteralPath $OutputDirectory).Path.TrimEnd('\')
if ([IO.Path]::GetFileName($ExecutableName) -ne $ExecutableName) {
    throw 'ExecutableName must be a file name, not a path.'
}

# UPX can rewrite PE headers/import tables. Verify actual code, data and resources after unpacking.
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

# Keep signed Qt/Windows DLLs and embedded third-party tools untouched.
$files = @($ExecutableName, 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')
$workspace = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
$scratch = [IO.Path]::GetFullPath((Join-Path $workspace ('build\upx-' + [Guid]::NewGuid().ToString('N'))))
if (-not $scratch.StartsWith($workspace + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'UPX scratch directory is outside the project.'
}
[IO.Directory]::CreateDirectory($scratch) | Out-Null
$scratchFiles = @()
$totalSaved = 0L
try {
    foreach ($file in $files) {
        $original = [IO.Path]::GetFullPath((Join-Path $output $file))
        if (-not $original.StartsWith($output + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Compression target outside deployment directory: $original"
        }
        $item = Get-Item -LiteralPath $original -ErrorAction Stop
        if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "UPX target is not a regular file: $original"
        }
        if ((Get-AuthenticodeSignature -LiteralPath $original).Status -ne 'NotSigned') {
            Write-Warning "UPX skipped signed file: $file"
            continue
        }
        $packed = Join-Path $scratch ($file + '.packed')
        $restored = Join-Path $scratch ($file + '.restored')
        $scratchFiles += @($packed, $restored)
        & $upx --best --lzma --strip-relocs=0 --compress-exports=0 --compress-icons=0 --compress-resources=0 -o $packed $original
        if ($LASTEXITCODE -ne 0) { throw "UPX compression failed: $file" }
        & $upx -t $packed
        if ($LASTEXITCODE -ne 0) { throw "UPX integrity check failed: $file" }
        & $upx -d -o $restored $packed
        if ($LASTEXITCODE -ne 0) { throw "UPX decompression check failed: $file" }
        $before = Get-PePayloadFingerprint $original
        $after = Get-PePayloadFingerprint $restored
        foreach ($section in $before.Keys) {
            if (-not $after.ContainsKey($section) -or $before[$section] -ne $after[$section]) {
                throw "UPX changed executable payload: $file / $section. Original kept."
            }
        }
        $saved = $item.Length - (Get-Item -LiteralPath $packed).Length
        if ($saved -gt 0) {
            Copy-Item -LiteralPath $packed -Destination $original -Force
            $totalSaved += $saved
            Write-Host "UPX verified $file; saved $saved bytes."
        } else {
            Write-Host "UPX has no benefit for $file; original kept."
        }
    }
} finally {
    # Only remove this invocation's known temporary files, never recursively remove arbitrary paths.
    foreach ($path in $scratchFiles) {
        $resolved = [IO.Path]::GetFullPath($path)
        if (-not $resolved.StartsWith($scratch + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Scratch cleanup target outside UPX directory: $resolved"
        }
        if (Test-Path -LiteralPath $resolved -PathType Leaf) {
            Remove-Item -LiteralPath $resolved -ErrorAction Stop
        }
    }
    if (@(Get-ChildItem -LiteralPath $scratch -Force).Count -eq 0) {
        Remove-Item -LiteralPath $scratch -ErrorAction Stop
    }
}
Write-Host "UPX complete; saved $totalSaved bytes. Signed Qt DLLs were not modified."
