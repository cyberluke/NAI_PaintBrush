<#
.SYNOPSIS
  Capture the already-running NAI Paintbrush window (attach mode).

.DESCRIPTION
  Unlike pb-capture-window.ps1 this never launches or copies an executable.
  It finds the visible top-level paintbrush process window and captures it at
  physical resolution (DPI-aware).

.EXAMPLE
  pwsh -File scripts/pb-capture-attached.ps1 -OutFile pb-workspace.png
#>
[CmdletBinding()]
param(
    [string]$ProcessName = "paintbrush",
    [string]$OutFile = "D:\_SATIN_AI\NAI-OS\vlc\pb-workspace.png",
    [ValidateSet("PrintWindow", "Screen")]
    [string]$Method = "Screen"
)

$ErrorActionPreference = "Stop"
Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class PbAttachWin32 {
  public delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
  [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  public static List<IntPtr> WindowsOf(uint pid) {
    var list = new List<IntPtr>();
    EnumWindows((h, l) => {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && IsWindowVisible(h)) list.Add(h);
      return true;
    }, IntPtr.Zero);
    return list;
  }
}
'@

$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
        Sort-Object StartTime -Descending | Select-Object -First 1
if (-not $proc) { throw "no $ProcessName process is running" }

[void][PbAttachWin32]::SetProcessDpiAwarenessContext([IntPtr](-4))
[void][PbAttachWin32]::SetThreadDpiAwarenessContext([IntPtr](-4))
Start-Sleep -Milliseconds 200

$target = [IntPtr]::Zero
$title = ""
foreach ($h in [PbAttachWin32]::WindowsOf([uint32]$proc.Id)) {
    $sb = New-Object System.Text.StringBuilder 512
    [void][PbAttachWin32]::GetWindowText($h, $sb, 512)
    if ($sb.ToString() -match "PaintBrush|Paintbrush|NAI") {
        $target = $h
        $title = $sb.ToString()
        break
    }
}
if ($target -eq [IntPtr]::Zero) { throw "no visible Paintbrush window for pid $($proc.Id)" }

$rect = New-Object PbAttachWin32+RECT
[void][PbAttachWin32]::GetWindowRect($target, [ref]$rect)
$w = $rect.Right - $rect.Left
$hgt = $rect.Bottom - $rect.Top
Write-Output "WINDOW|pid=$($proc.Id)|title=$title|size=${w}x${hgt}"

Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
if ($Method -eq "Screen") {
    [void][PbAttachWin32]::SetForegroundWindow($target)
    Start-Sleep -Milliseconds 400
    $g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bmp.Size)
} else {
    $hdc = $g.GetHdc()
    [void][PbAttachWin32]::PrintWindow($target, $hdc, 0)
    $g.ReleaseHdc($hdc)
}
$g.Dispose()
$bmp.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "CAPTURE|path=$OutFile|size=${w}x${hgt}|title=$title"
