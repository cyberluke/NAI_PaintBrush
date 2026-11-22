# Register NAI PaintBrush as a real Windows app entry.
#
# Creates/updates a per-user Start Menu shortcut so the shell, the NAI OS dock
# and the app menu can resolve the product name, icon and launch arguments for
# nai-paintbrush.exe. Without it, surfaces that identify apps by shortcut/exe
# (Seelen/NAI OS dock, focused-app header) fall back to the old VLC identity
# after the executable rename.
#
#   pwsh scripts/pb-install-shortcut.ps1
#   pwsh scripts/pb-install-shortcut.ps1 -Uninstall
#
# No machine-wide changes, no installer, no registry writes.
[CmdletBinding()]
param(
    [string]$Exe = "",
    [string]$ShortcutName = "NAI PaintBrush.lnk",
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $root 'win64-meson\bin\nai-paintbrush.exe' }
$icon = Join-Path $root 'share\icons\nai-paintbrush.ico'
$startMenu = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
$lnk = Join-Path $startMenu $ShortcutName

if ($Uninstall) {
    if (Test-Path -LiteralPath $lnk) {
        Remove-Item -LiteralPath $lnk -Force
        Write-Host "Removed $lnk"
    } else {
        Write-Host "Nothing to remove: $lnk"
    }
    exit 0
}

if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    throw "Build output not found: $Exe`nBuild it first: ninja -C win64-meson bin/nai-paintbrush.exe"
}
if (-not (Test-Path -LiteralPath $icon -PathType Leaf)) {
    throw "Icon not found: $icon`nGenerate it: python scripts/pb-make-icon.py <square.png>"
}

$shell = New-Object -ComObject WScript.Shell
$sc = $shell.CreateShortcut($lnk)
$sc.TargetPath = (Resolve-Path -LiteralPath $Exe).Path
$sc.Arguments = '--paintbrush'
$sc.WorkingDirectory = Split-Path -Parent (Resolve-Path -LiteralPath $Exe).Path
$sc.IconLocation = "$((Resolve-Path -LiteralPath $icon).Path),0"
$sc.Description = 'NAI PaintBrush'
$sc.WindowStyle = 1
$sc.Save()

Write-Host "Start Menu entry: $lnk"
Write-Host "  target: $($sc.TargetPath) $($sc.Arguments)"
Write-Host "  icon:   $($sc.IconLocation)"
Write-Host ""
Write-Host "If a surface (NAI OS dock / app menu) still shows the old VLC entry,"
Write-Host "unpin that old item and pin 'NAI PaintBrush' from the app menu."
