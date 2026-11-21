<#
.SYNOPSIS
  Capture the NAI Paintbrush native window to a PNG (offscreen-safe).

.DESCRIPTION
  The paintbrush workspace is a native Qt window whose chrome (bottom strip)
  can sit below the visible desktop area on a scaled display, so a plain
  CopyFromScreen screenshot is not a reliable GUI gate. This script launches
  the built paintbrush.exe, finds the real visible top-level window that
  belongs to the process (not a hidden Qt helper), and renders it with
  PrintWindow(PW_RENDERFULLCONTENT) into a PNG at the window's own size.

  Reusable by anyone verifying a Paintbrush UI change:
    pwsh -File scripts/pb-capture-window.ps1 -OutFile C:\tmp\pb.png

.PARAMETER Exe
  paintbrush.exe (defaults to win64-meson\bin\paintbrush.exe).

.PARAMETER OutFile
  Destination PNG (defaults to <repo>\pb-window.png).

.PARAMETER WaitSeconds
  Seconds to let the workspace settle before capture (default 9).

.PARAMETER ExtraArgs
  Additional VLC/Qt command line arguments.

.PARAMETER Keys
  Optional SendKeys sequence sent to the workspace after it settles and before
  the capture (e.g. "o" to switch to Object Select). Used to verify tool-state
  UI without a human at the keyboard.
#>
[CmdletBinding()]
param(
    [string]$Exe = "D:\_SATIN_AI\NAI-OS\vlc\win64-meson\bin\paintbrush.exe",
    [string]$OutFile = "D:\_SATIN_AI\NAI-OS\vlc\pb-window.png",
    [int]$WaitSeconds = 9,
    [string[]]$ExtraArgs = @(),
    [string]$Keys = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "win64-meson\bin"
if (-not (Test-Path -LiteralPath $Exe)) {
    $Exe = Join-Path $bin "paintbrush.exe"
}
if (-not (Test-Path -LiteralPath $Exe)) {
    Copy-Item -LiteralPath (Join-Path $bin "vlc.exe") -Destination (Join-Path $bin "paintbrush.exe") -Force
    $Exe = Join-Path $bin "paintbrush.exe"
}

$env:VLC_PLUGIN_PATH = Join-Path $root "win64-meson\modules"
$env:QT_PLUGIN_PATH = "C:\msys64\mingw64\share\qt6\plugins"
$env:QML_IMPORT_PATH = "C:\msys64\mingw64\share\qt6\qml"
$env:QML2_IMPORT_PATH = $env:QML_IMPORT_PATH
$env:PATH = "$bin;C:\msys64\mingw64\bin;$(Join-Path $root 'win64-meson\lib');" +
            "$(Join-Path $root 'win64-meson\src');" + $env:PATH

Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class PbWin32 {
  public delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
  [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
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

$existing = Get-Process -Name paintbrush -ErrorAction SilentlyContinue
if ($existing) {
    Write-Warning "paintbrush is already running; close it first so this capture is deterministic"
}

$log = Join-Path $env:TEMP "pb-capture-vlc.log"
$args = @(
    "--paintbrush", "--no-media-library", "--no-qt-privacy-ask",
    "--no-qt-updates-notif", "--no-qt-name-in-title", "--no-one-instance",
    "--verbose=2", "--file-logging", "--logmode=text", "--logfile=$log",
    "-I", "qt"
) + $ExtraArgs

$proc = Start-Process -FilePath $Exe -ArgumentList $args -WorkingDirectory $bin -PassThru
try {
    Start-Sleep -Seconds $WaitSeconds

    $target = [IntPtr]::Zero
    $title = ""
    foreach ($h in [PbWin32]::WindowsOf([uint32]$proc.Id)) {
        $sb = New-Object System.Text.StringBuilder 512
        [void][PbWin32]::GetWindowText($h, $sb, 512)
        if ($sb.ToString() -match "PaintBrush|Paintbrush") {
            $target = $h
            $title = $sb.ToString()
            break
        }
    }
    if ($target -eq [IntPtr]::Zero) {
        throw "No visible Paintbrush window found for pid $($proc.Id)"
    }

    # The paintbrush window renders at 2x on this 4K display. Without a
    # DPI-aware caller, GetWindowRect/PrintWindow are virtualized and silently
    # capture only the top-left quarter of the window — which is exactly how a
    # bottom strip can appear "missing" in evidence while being present.
    # PER_MONITOR_AWARE_V2 (context -4) makes the capture cover the real
    # window at its physical size.
    [void][PbWin32]::SetProcessDpiAwarenessContext([IntPtr](-4))
    [void][PbWin32]::SetThreadDpiAwarenessContext([IntPtr](-4))
    Start-Sleep -Milliseconds 250

    if ($Keys -ne "") {
        Add-Type -AssemblyName Microsoft.VisualBasic
        Add-Type -AssemblyName System.Windows.Forms
        try {
            [void][PbWin32]::ShowWindow($target, 9)     # SW_RESTORE
            [void][PbWin32]::SetForegroundWindow($target)
            try { [void][Microsoft.VisualBasic.Interaction]::AppActivate($proc.Id) } catch {}
            Start-Sleep -Milliseconds 600
            [System.Windows.Forms.SendKeys]::SendWait($Keys)
            Start-Sleep -Milliseconds 1200
        }
        catch {
            Write-Warning "Could not send keys '$Keys': $_"
        }
    }

    $r = New-Object PbWin32+RECT
    [void][PbWin32]::GetWindowRect($target, [ref]$r)
    $w = $r.Right - $r.Left
    $hgt = $r.Bottom - $r.Top
    if ($w -le 0 -or $hgt -le 0) {
        throw "Window has invalid size ${w}x${hgt}"
    }

    Add-Type -AssemblyName System.Drawing
    $bmp = New-Object System.Drawing.Bitmap $w, $hgt
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][PbWin32]::PrintWindow($target, $hdc, 2)  # PW_RENDERFULLCONTENT
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    $bmp.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()

    Write-Output "CAPTURE|path=$OutFile|size=${w}x${hgt}|title=$title|log=$log"
}
finally {
    if ($proc -and -not $proc.HasExited) {
        $proc.Kill()
        Start-Sleep -Milliseconds 500
    }
}
