[CmdletBinding()]
param(
    [string]$ReleaseDirectory = (Join-Path $PSScriptRoot 'OrangeToolsApp'),
    [string]$ArchivePath = (Join-Path $PSScriptRoot 'dist\OrangeTools-Portable.7z'),
    [string]$SevenZipPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'scripts\Protection.Common.ps1')

# Find an installed 7-Zip; do not download or install tools automatically.
if ([string]::IsNullOrWhiteSpace($SevenZipPath)) {
    $command = Get-Command 7z.exe -ErrorAction SilentlyContinue
    if ($command) { $SevenZipPath = $command.Source }
    if ([string]::IsNullOrWhiteSpace($SevenZipPath)) {
        foreach ($key in @('HKLM:\SOFTWARE\7-Zip', 'HKCU:\SOFTWARE\7-Zip')) {
            $install = Get-ItemPropertyValue -LiteralPath $key -Name Path -ErrorAction SilentlyContinue
            if ($install -and (Test-Path -LiteralPath (Join-Path $install '7z.exe') -PathType Leaf)) {
                $SevenZipPath = Join-Path $install '7z.exe'
                break
            }
        }
    }
    if ([string]::IsNullOrWhiteSpace($SevenZipPath)) {
        foreach ($install in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
            if (-not $install) { continue }
            $candidate = Join-Path $install '7-Zip\7z.exe'
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { $SevenZipPath = $candidate; break }
        }
    }
}
if ([string]::IsNullOrWhiteSpace($SevenZipPath)) {
    throw '7-Zip not found. Pass -SevenZipPath pointing to an installed 7z.exe.'
}
$sevenZip = (Resolve-Path -LiteralPath $SevenZipPath).Path
$release = (Resolve-Path -LiteralPath $ReleaseDirectory).Path.TrimEnd('\')
Assert-ReleasePayload $release
$archive = [IO.Path]::GetFullPath($ArchivePath)
if ([IO.Path]::GetExtension($archive) -ne '.7z') { throw 'ArchivePath must end in .7z.' }
if ($archive.StartsWith($release + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The archive must be outside the release directory.'
}
if ((Test-Path -LiteralPath $archive) -or (Test-Path -LiteralPath ($archive + '.sha256'))) {
    throw 'Output already exists. Choose a new -ArchivePath; existing packages are never overwritten.'
}
$rootEntry = Get-Item -LiteralPath $release -Force
$entries = @(Get-ChildItem -LiteralPath $release -Recurse -Force)
if (($rootEntry.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
    @($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) {
    throw 'The release directory must not contain symlinks or junctions.'
}
$required = @('Orange Tools.exe', 'qt.conf', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll',
              'Qt6Network.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll',
              'platforms\qwindows.dll', 'styles\qmodernwindowsstyle.dll',
              'networkinformation\qnetworklistmanager.dll', 'tls\qschannelbackend.dll')
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $release $relative) -PathType Leaf)) {
        throw "Incomplete deployment: $relative is missing. Run deploy.ps1 first."
    }
}
$files = @($entries | Where-Object { -not $_.PSIsContainer })
if (@($files | Where-Object { $_.Name -in @('_deployment_smoke.exe', 'Qt6Test.dll') }).Count) {
    throw 'Temporary test files remain in the release directory; do not distribute them.'
}
$before = @{}
foreach ($file in $files) { $before[$file.FullName] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash }
$unpackedBytes = ($files | Measure-Object -Property Length -Sum).Sum
$parent = Split-Path -Parent $release
$folder = Split-Path -Leaf $release
[IO.Directory]::CreateDirectory((Split-Path -Parent $archive)) | Out-Null

# The dictionary covers this release; solid compression shares data across DLLs.
Push-Location -LiteralPath $parent
try {
    & $sevenZip a -t7z $archive $folder -mx=9 -ms=on -md=64m -mfb=273 -mmt=2 -mtc=off -mta=off -spd -bsp0 -sccUTF-8
    if ($LASTEXITCODE -ne 0) { throw '7-Zip packaging failed. Do not distribute this archive.' }
} finally { Pop-Location }
& $sevenZip t $archive -bsp0 -sccUTF-8
if ($LASTEXITCODE -ne 0) { throw 'Archive integrity check failed. Do not distribute this archive.' }

# Hash decompressed bytes directly from stdout: no temporary extraction or application launch.
function Get-ArchivedFileHash([string]$Member) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $sevenZip
    $info.Arguments = 'x -so -y -bd -bb0 -spd -sccUTF-8 "' + $archive + '" "' + $Member + '"'
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        [void]$process.Start()
        $errors = $process.StandardError.ReadToEndAsync()
        $hash = [BitConverter]::ToString($hasher.ComputeHash($process.StandardOutput.BaseStream)).Replace('-', '')
        $process.WaitForExit()
        $diagnostic = $errors.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) { throw "Archive extraction failed for $Member : $diagnostic" }
        return $hash
    } finally { $hasher.Dispose(); $process.Dispose() }
}
foreach ($file in $files) {
    $relative = $file.FullName.Substring($release.Length + 1)
    $member = $folder + '\' + $relative
    if ((Get-ArchivedFileHash $member) -ne $before[$file.FullName]) {
        throw "Archive content mismatch: $relative. Do not distribute this archive."
    }
    if ((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash -ne $before[$file.FullName]) {
        throw "Release file changed during packaging: $relative. Package again."
    }
}
$archiveHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
[IO.File]::WriteAllText($archive + '.sha256', $archiveHash + '  ' + [IO.Path]::GetFileName($archive) + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
$archiveBytes = (Get-Item -LiteralPath $archive).Length
Write-Host "Verified $($files.Count) files against their original SHA-256; release directory unchanged."
Write-Host "Unpacked: $unpackedBytes bytes; download: $archiveBytes bytes."
Write-Host "Download package: $archive"
Write-Host 'Extract the entire archive, then run OrangeToolsApp/Orange Tools.exe. Do not run inside the archive.'
