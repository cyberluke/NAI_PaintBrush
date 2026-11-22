# Powder live-preview acceptance smoke (GUI).
# Focuses the running Paintbrush window, selects Glitter Powder (Shift+L),
# drags a horizontal stroke on the canvas, waits for the commit bake, captures
# the window and prints the powder log evidence (renderer + perf lines).
#
#   pwsh -File scripts/pb-powder-verify.ps1
#   pwsh -File scripts/pb-powder-verify.ps1 -NoBlow          # skip the long-press
#
# Evidence this produces:
#   "pb powder: live preview renderer = gpu-qsg (zero readback)"  -> node active
#   "pb powder: live preview renderer = cpu material"            -> fallback
#   "POWDER PERF gpu=1 physics=..ms pack=..ms pack_bytes=.."     -> cost split
[CmdletBinding()]
param(
    [string]$ProcessName = "paintbrush",
    [string]$OutFile = "D:\_SATIN_AI\NAI-OS\vlc\_REVIEW\pb-powder-verify.png",
    [string]$LogFile = "D:\_SATIN_AI\NAI-OS\vlc\pb_noskip.log",
    [switch]$NoBlow,
    [switch]$SkipToolSelect,
    [double]$YFraction = 0.62,
    [double]$X0Fraction = 0.30,
    [double]$X1Fraction = 0.55
)

$ErrorActionPreference = "Stop"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class PbVerify {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
'@
[void][PbVerify]::SetProcessDpiAwarenessContext([IntPtr](-4))

$p = Get-Process -Name $ProcessName -ErrorAction Stop |
     Where-Object { $_.MainWindowHandle -ne 0 } |
     Sort-Object StartTime -Descending | Select-Object -First 1
$h = $p.MainWindowHandle
[PbVerify]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 600

$r = New-Object PbVerify+RECT
[void][PbVerify]::GetWindowRect($h, [ref]$r)
$w = $r.Right - $r.Left
$ht = $r.Bottom - $r.Top
if ($w -lt 400 -or $ht -lt 300) { throw "window too small: ${w}x${ht}" }

$y  = [int]($r.Top + $ht * $YFraction)
$x0 = [int]($r.Left + $w * $X0Fraction)
$x1 = [int]($r.Left + $w * $X1Fraction)

if (-not $SkipToolSelect) {
    [PbVerify]::keybd_event(0x10, 0, 0, [IntPtr]::Zero)      # Shift down
    [PbVerify]::keybd_event(0x4C, 0, 0, [IntPtr]::Zero)      # L down
    [PbVerify]::keybd_event(0x4C, 0, 2, [IntPtr]::Zero)      # L up
    [PbVerify]::keybd_event(0x10, 0, 2, [IntPtr]::Zero)      # Shift up
    Start-Sleep -Milliseconds 500
}

Write-Output ("window {0}x{1} drag ({2},{3}) -> ({4},{3})" -f $w, $ht, $x0, $y, $x1)

# one steady stroke: 40 steps over ~1.2 s
[void][PbVerify]::SetCursorPos($x0, $y)
Start-Sleep -Milliseconds 150
[PbVerify]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
$steps = 40
for ($i = 1; $i -le $steps; ++$i) {
    $x = [int]($x0 + ($x1 - $x0) * $i / $steps)
    [void][PbVerify]::SetCursorPos($x, $y)
    Start-Sleep -Milliseconds 30
}
if (-not $NoBlow) {
    # hold still: the reference's blowOnLongPress breath
    Start-Sleep -Milliseconds 900
}
[PbVerify]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Seconds 1

& (Join-Path $PSScriptRoot "pb-capture-attached.ps1") -OutFile $OutFile
Write-Output ("capture: " + $OutFile)

if (Test-Path -LiteralPath $LogFile) {
    Write-Output "--- powder log evidence ---"
    Get-Content -LiteralPath $LogFile |
        Select-String -Pattern 'pb powder: (stroke begin|live preview)|POWDER PERF|QSG powder node failed' |
        Select-Object -Last 12 |
        ForEach-Object { $_.Line }
}
