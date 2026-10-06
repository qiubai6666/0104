# Loaded after SingleExe.Common.ps1. Never edit the pinned module or finished SFX.
function New-IconSfxModule([string]$Module, [string]$Icon, [string]$Output) {
    Assert-NoLinks $Module
    Assert-NoLinks $Icon
    Assert-NoLinks $Output
    if (Test-Path -LiteralPath $Output) { throw "Refusing to overwrite icon module: $Output" }
    if (-not ('OrangeSfxIcon' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'SfxIcon.cs') }
    Copy-Item -LiteralPath $Module -Destination $Output -ErrorAction Stop
    [OrangeSfxIcon]::Apply($Output, $Icon)
    return $Output
}