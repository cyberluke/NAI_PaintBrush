<#
.SYNOPSIS
  Capture the window of an ALREADY RUNNING NAI Paintbrush process.

.DESCRIPTION
  pb-capture-window.ps1 launches its own instance so it is deterministic for
  static UI checks. Interactive checks (open the Play Pad, press Make it real,
  close the pad) need to capture the live process without restarting it. This
  script finds the visible Paintbrush top-level window for a running process,
  switches the caller to per-monitor DPI awareness and renders the window with
  PrintWindow(PW_RENDERFULLCONTENT) at its physical size.

  Reusable example:
    pwsh -File scripts/pb-capture-live.ps1 -OutFile D:\tmp\pb-live.png
    pwsh -File scripts/pb-capture-live.ps1 -ProcessId 12345 -OutFile ...
#>
[CmdletBinding()]
param(
    [int]$ProcessId = 0,
    [string]$OutFile = "D:\_SATIN_AI\NAI-OS\vlc\pb-live.png",
    [string]$TitleMatch = "PaintBrush|Paintbrush"
)

$ErrorActionPreference = "Stop"

Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class PbLiveCap {
  public delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
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

Add-Type -AssemblyName System.Drawing

$proc = $null
if ($ProcessId -ne 0) {
    $proc = Get-Process -Id $ProcessId -ErrorAction Stop
} else {
    $proc = Get-Process -Name nai-paintbrush -ErrorAction SilentlyContinue |
            Where-Object { $_.MainWindowHandle -ne 0 } |
            Select-Object -First 1
}
if (-not $proc) {
    throw "No running nai-paintbrush window found"
}

[void][PbLiveCap]::SetProcessDpiAwarenessContext([IntPtr](-4))

$target = [IntPtr]::Zero
foreach ($h in [PbLiveCap]::WindowsOf([uint32]$proc.Id)) {
    $sb = New-Object System.Text.StringBuilder 512
    [void][PbLiveCap]::GetWindowText($h, $sb, 512)
    if ($sb.ToString() -match $TitleMatch) {
        $target = $h
        break
    }
}
if ($target -eq [IntPtr]::Zero) {
    throw "No visible Paintbrush window for pid $($proc.Id)"
}

$r = New-Object PbLiveCap+RECT
[void][PbLiveCap]::GetWindowRect($target, [ref]$r)
$w = $r.Right - $r.Left
$h = $r.Bottom - $r.Top
if ($w -le 0 -or $h -le 0) {
    throw "Window has an empty rect"
}

$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$ok = [PbLiveCap]::PrintWindow($target, $hdc, 2)
$g.ReleaseHdc($hdc)
$g.Dispose()
$bmp.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

"pid=$($proc.Id) ${w}x${h} PrintWindow=$ok -> $OutFile"
