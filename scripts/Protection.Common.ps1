# Read-only inspection and explicit finalization of task-owned Release artifacts.
. (Join-Path $PSScriptRoot 'SingleExe.Common.ps1')

function Get-ReleaseLeaks([string]$Root, [string[]]$Files) {
    Assert-NoLinks $Root
    if (-not $PSBoundParameters.ContainsKey('Files')) {
        $Files = @(Get-ChildItem -LiteralPath $Root -Recurse -File -Force | ForEach-Object FullName)
    }
    foreach ($file in $Files) {
        $relative = [IO.Path]::GetRelativePath($Root, $file)
        if ($relative.StartsWith('..') -or [IO.Path]::IsPathRooted($relative)) { throw "Audit member outside root: $file" }
        $extension = [IO.Path]::GetExtension($file).ToLowerInvariant()
        $name = [IO.Path]::GetFileName($file)
        # Notices and legally required third-party source are retained under licenses.
        # Debug/build artifacts are never allowed, even under licenses.
        $source = $extension -in @('.c','.cc','.cpp','.cxx','.h','.hpp','.pro','.pri','.ps1','.cmake','.qrc','.ui','.qbs','.vcxproj','.sln','.rc') -or
                  $name -match '^(Makefile($|\.)|CMakeLists\.txt$)'
        $artifact = $extension -in @('.o','.obj','.pdb','.debug','.dmp','.map','.ilk','.a','.lib','.log') -or
                    $relative -match '(^|[\\/])(\.git|\.qtcreator)([\\/]|$)'
        if ($artifact -or ($source -and $relative -notmatch '^licenses[\\/]')) { $relative }
    }
}
function Assert-ReleasePayload([string]$Root, [string[]]$Files) {
    $arguments = @{ Root=$Root }
    if ($PSBoundParameters.ContainsKey('Files')) { $arguments.Files = $Files }
    $leaks = @(Get-ReleaseLeaks @arguments)
    if ($leaks.Count) { throw "Source/debug/build files in generated deployment: $($leaks -join ', ')" }
}

function Initialize-ProtectionScanner {
    if ('OrangeProtection.BinaryScan' -as [type]) { return }
    Add-Type -TypeDefinition @"
namespace OrangeProtection {
    public static class BinaryScan {
        public static bool Contains(byte[] data, byte[] needle) {
            if (needle.Length == 0) return false;
            int position = 0;
            while (position <= data.Length - needle.Length) {
                position = System.Array.IndexOf(data, needle[0], position);
                if (position < 0 || position > data.Length - needle.Length) return false;
                int index = 1;
                while (index < needle.Length && data[position + index] == needle[index]) ++index;
                if (index == needle.Length) return true;
                ++position;
            }
            return false;
        }
    }
}
"@
}
function Get-ProtectionInventory([string]$SourceRoot) {
    $header = Join-Path $SourceRoot 'src\app\sensitivestrings.h'
    foreach ($line in Get-Content -LiteralPath $header -Encoding utf8) {
        if ($line -match '^ORANGE_SENSITIVE_STRING\((\w+),\s*("(?:[^"\\]|\\.)*")\)$') {
            [pscustomobject]@{ Name=$Matches[1]; Text=($Matches[2] | ConvertFrom-Json) }
        }
    }
}
function Assert-ProtectedBinary([string]$Executable, [string]$SourceRoot) {
    Initialize-ProtectionScanner
    $bytes = [IO.File]::ReadAllBytes($Executable)
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) { throw 'Not a PE executable' }
    $pe = [BitConverter]::ToInt32($bytes, 0x3c)
    if ($pe -lt 64 -or $pe + 24 -gt $bytes.Length -or [BitConverter]::ToUInt32($bytes,$pe) -ne 0x4550) { throw 'Invalid PE header' }
    # A COFF string table is needed for long section names (.gnu_debuglink), not symbols.
    if ([BitConverter]::ToUInt32($bytes,$pe+16) -ne 0) { throw 'COFF symbols remain in protected executable' }
    $inventory = @(Get-ProtectionInventory $SourceRoot)
    if ($inventory.Count -lt 1) { throw 'Sensitive string inventory is empty' }
    $paths = @($SourceRoot, $SourceRoot.Replace('\','/')) | Select-Object -Unique
    foreach ($encoding in @([Text.Encoding]::UTF8, [Text.Encoding]::Unicode)) {
        foreach ($entry in $inventory) {
            if ([OrangeProtection.BinaryScan]::Contains($bytes, $encoding.GetBytes($entry.Text))) { throw "Sensitive plaintext remains ($($encoding.WebName)): $($entry.Name)" }
        }
        foreach ($path in $paths) {
            if ([OrangeProtection.BinaryScan]::Contains($bytes, $encoding.GetBytes($path))) { throw 'Absolute source path remains in protected executable' }
        }
    }
    return @($inventory | ForEach-Object Name)
}
function Finalize-ProtectedExecutable([string]$Executable, [string]$BuildDirectory, [string]$CompilerBinPath,
                                      [string]$PrivateDirectory, [string]$SourceRoot) {
    foreach ($path in @($Executable,$BuildDirectory,$PrivateDirectory,$SourceRoot)) { Assert-NoLinks $path }
    $projectTemp = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $PSScriptRoot) 'TEMP')).TrimEnd('\') + '\'
    foreach ($path in @($Executable, $PrivateDirectory)) {
        if (-not [IO.Path]::GetFullPath($path).StartsWith($projectTemp,[StringComparison]::OrdinalIgnoreCase)) { throw 'Finalization must stay inside project TEMP' }
    }
    if (Test-Path -LiteralPath $PrivateDirectory) { throw 'Refusing to reuse private symbol directory' }
    $makefile = Join-Path $BuildDirectory 'Makefile.Release'
    $make = [IO.File]::ReadAllText($makefile)
    foreach ($pattern in @('(?m)^CXXFLAGS\s*=.*-flto','(?m)^CXXFLAGS\s*=.*-O2','(?m)^CXXFLAGS\s*=.*(?:\s)-g(?:\s)',
                           '(?m)^LFLAGS\s*=.*-flto','ORANGE_PROTECTED_RELEASE')) {
        if ($make -notmatch $pattern) { throw "Missing protected build flag: $pattern" }
    }
    if ($make -match '(?m)^LFLAGS\s*=.*(?:-Wl,-s\b|\s-s\b)') { throw 'Linker stripped symbols before extraction' }
    foreach ($tool in @('objcopy.exe','objdump.exe','strip.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $CompilerBinPath $tool))) { throw "Missing matching binutils: $tool" }
    }
    [IO.Directory]::CreateDirectory($PrivateDirectory) | Out-Null
    $debug = Join-Path $PrivateDirectory 'OrangeTools.debug'
    $objcopy = Join-Path $CompilerBinPath 'objcopy.exe'
    $objdump = Join-Path $CompilerBinPath 'objdump.exe'
    Invoke-ReleaseTool $objcopy @('--only-keep-debug',$Executable,$debug) (Join-Path $PrivateDirectory 'extract.log')
    $debugSections = Join-Path $PrivateDirectory 'symbols-sections.log'
    Invoke-ReleaseTool $objdump @('-h',$debug) $debugSections
    if ([IO.File]::ReadAllText($debugSections) -notmatch '\.debug_info') { throw 'No DWARF debug information extracted' }
    Invoke-ReleaseTool (Join-Path $CompilerBinPath 'strip.exe') @('--strip-all',$Executable) (Join-Path $PrivateDirectory 'strip.log')
    Invoke-ReleaseTool $objcopy @("--add-gnu-debuglink=$debug",$Executable) (Join-Path $PrivateDirectory 'debuglink.log')
    $sections = Join-Path $PrivateDirectory 'release-sections.log'
    Invoke-ReleaseTool $objdump @('-h',$Executable) $sections
    $sectionText = [IO.File]::ReadAllText($sections)
    if ($sectionText -match '\.debug_') { throw 'Debug sections remain in protected executable' }
    if ($sectionText -notmatch '\.gnu_debuglink') { throw 'Private symbol link missing' }
    $names = @(Assert-ProtectedBinary $Executable $SourceRoot)
    $manifest = [pscustomobject]@{
        Executable=[IO.Path]::GetFileName($Executable); Sha256=(Get-FileHash -LiteralPath $Executable).Hash
        DebugFile=[IO.Path]::GetFileName($debug); DebugSha256=(Get-FileHash -LiteralPath $debug).Hash
        CompilerBinPath=$CompilerBinPath; SensitiveStrings=$names; SourceRoot=$SourceRoot
    }
    $manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $PrivateDirectory 'protection.json') -Encoding utf8NoBOM
    return Join-Path $PrivateDirectory 'protection.json'
}
function Assert-ProtectionManifest([string]$Executable, [string]$ManifestPath) {
    Assert-NoLinks $ManifestPath
    $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    if ($manifest.Executable -ne [IO.Path]::GetFileName($Executable) -or $manifest.Sha256 -ne (Get-FileHash -LiteralPath $Executable).Hash) { throw 'Protected executable does not match verified manifest' }
}

# Some legacy BAT regressions require an ASCII ROM path even when 8.3 names
# are disabled. A temporary DOS alias points only to our checked TEMP
# directory; no data is moved and existing drive mappings are never replaced.
function Initialize-ReleaseTestDrive {
    if ('OrangeProtection.TestDrive' -as [type]) { return }
    Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
namespace OrangeProtection {
    public static class TestDrive {
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        private static extern uint QueryDosDevice(string name, StringBuilder target, int size);
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        private static extern bool DefineDosDevice(uint flags, string name, string target);
        public static string Target(string name) {
            var target = new StringBuilder(32768);
            if (QueryDosDevice(name, target, target.Capacity) != 0) return target.ToString();
            int error = Marshal.GetLastWin32Error();
            if (error == 2) return null; // ERROR_FILE_NOT_FOUND, not an occupied drive.
            throw new System.ComponentModel.Win32Exception(error);
        }
        public static void Create(string name, string target) {
            if (!DefineDosDevice(9, name, target))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        }
        public static void Remove(string name, string target) {
            if (!String.Equals(Target(name), target, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("Test drive mapping changed; refusing removal");
            if (!DefineDosDevice(15, name, target))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        }
    }
}
"@
}
function New-ReleaseAsciiTestHome([string]$Directory) {
    $full = [IO.Path]::GetFullPath($Directory).TrimEnd('\')
    $temp = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $PSScriptRoot) 'TEMP')).TrimEnd('\') + '\'
    if (-not $full.StartsWith($temp,[StringComparison]::OrdinalIgnoreCase)) { throw 'Test drive target must stay inside project TEMP' }
    Assert-NoLinks $full
    if (-not (Test-Path -LiteralPath $full -PathType Container)) { throw 'Test home does not exist' }
    Initialize-ReleaseTestDrive
    foreach ($number in 90..69) {
        $drive = [string][char]$number + ':'
        if (-not [OrangeProtection.TestDrive]::Target($drive)) {
            $target = '\??\' + $full
            [OrangeProtection.TestDrive]::Create($drive,$target)
            $mapping = [pscustomobject]@{ Drive=$drive; Target=$target; Home=$drive+'\' }
            if (-not [IO.Directory]::Exists($mapping.Home)) {
                [OrangeProtection.TestDrive]::Remove($drive,$target)
                throw 'Test drive alias is not accessible'
            }
            return $mapping
        }
    }
    throw 'No free drive letter for an isolated ASCII test path'
}
function Remove-ReleaseAsciiTestHome($Mapping) {
    if ($Mapping) { [OrangeProtection.TestDrive]::Remove($Mapping.Drive,$Mapping.Target) }
}