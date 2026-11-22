# Register NAI PaintBrush as a real Windows app entry.
#
# Creates/updates a per-user Start Menu shortcut so the shell, the NAI OS dock
# and the app menu can resolve the product name, icon, launch arguments and
# AppUserModelID for nai-paintbrush.exe.
#
# Windows has THREE identity layers for an unpackaged app:
#   1. the process AUMID     SetCurrentProcessExplicitAppUserModelID("NAI.PaintBrush")
#                            (qt.cpp, before the first window)
#   2. the shortcut AUMID    System.AppUserModel.ID on the .lnk (this script)
#   3. the surface's own pin  NAI OS dock / Start pin (user action)
# If (2) is missing, Windows derives it from the target path, so the shortcut
# and the running process land in different groups and icon caches.
#
#   pwsh scripts/pb-install-shortcut.ps1
#   pwsh scripts/pb-install-shortcut.ps1 -Uninstall
#
# No machine-wide changes, no installer, no registry writes.
[CmdletBinding()]
param(
    [string]$Exe = "",
    [string]$ShortcutName = "NAI PaintBrush.lnk",
    [string]$AppId = "NAI.PaintBrush",
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

# --- base shortcut ---------------------------------------------------------
$shell = New-Object -ComObject WScript.Shell
$sc = $shell.CreateShortcut($lnk)
$sc.TargetPath = (Resolve-Path -LiteralPath $Exe).Path
$sc.Arguments = '--paintbrush'
$sc.WorkingDirectory = Split-Path -Parent (Resolve-Path -LiteralPath $Exe).Path
$sc.IconLocation = "$((Resolve-Path -LiteralPath $icon).Path),0"
$sc.Description = 'NAI PaintBrush'
$sc.WindowStyle = 1
$sc.Save()
$targetText = "$($sc.TargetPath) $($sc.Arguments)"
$iconText = $sc.IconLocation
[void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($sc)
[void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($shell)
Start-Sleep -Milliseconds 300

# --- AppUserModelID on the shortcut ---------------------------------------
# WScript.Shell cannot write it; IPropertyStore can.
#   SHGetPropertyStoreFromParsingName(path, NULL, flags, REFIID, void **ppv)
# PKEY_AppUserModel_ID = {9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3}, pid 5.
if (-not ('PbLnkAumid' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class PbLnkAumid
{
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    private struct PROPERTYKEY { public Guid fmtid; public uint pid; }

    [StructLayout(LayoutKind.Explicit)]
    private struct PROPVARIANT
    {
        [FieldOffset(0)] public ushort vt;
        [FieldOffset(8)] public IntPtr pwszVal;
    }

    [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IPropertyStore
    {
        uint GetCount();
        void GetAt(uint iProp, out PROPERTYKEY pkey);
        void GetValue(ref PROPERTYKEY key, out PROPVARIANT pv);
        void SetValue(ref PROPERTYKEY key, ref PROPVARIANT pv);
        void Commit();
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode, PreserveSig = true)]
    private static extern int SHGetPropertyStoreFromParsingName(
        string pszPath, IntPtr pbc, uint flags, ref Guid riid,
        [MarshalAs(UnmanagedType.Interface)] out IPropertyStore ppv);

    private static readonly Guid IID_IPropertyStore =
        new Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99");
    private static readonly Guid FMTID_AppUserModel =
        new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3");
    private const ushort VT_LPWSTR = 31;
    private const uint GPS_READWRITE = 2;
    private const uint GPS_READONLY = 0;

    private static IPropertyStore Open(string lnkPath, uint flags)
    {
        var iid = IID_IPropertyStore;
        IPropertyStore store;
        int hr = SHGetPropertyStoreFromParsingName(
            lnkPath, IntPtr.Zero, flags, ref iid, out store);
        if (hr < 0)
            Marshal.ThrowExceptionForHR(hr);
        return store;
    }

    public static void Set(string lnkPath, string appId)
    {
        var key = new PROPERTYKEY { fmtid = FMTID_AppUserModel, pid = 5 };
        var pv = new PROPVARIANT
        {
            vt = VT_LPWSTR,
            pwszVal = Marshal.StringToCoTaskMemUni(appId)
        };
        try
        {
            IPropertyStore store = Open(lnkPath, GPS_READWRITE);
            store.SetValue(ref key, ref pv);
            store.Commit();
            Marshal.ReleaseComObject(store);
        }
        finally
        {
            Marshal.FreeCoTaskMem(pv.pwszVal);
        }
    }

    public static string Get(string lnkPath)
    {
        var key = new PROPERTYKEY { fmtid = FMTID_AppUserModel, pid = 5 };
        IPropertyStore store = Open(lnkPath, GPS_READONLY);
        PROPVARIANT pv;
        store.GetValue(ref key, out pv);
        Marshal.ReleaseComObject(store);
        if (pv.vt != VT_LPWSTR || pv.pwszVal == IntPtr.Zero)
            return "";
        try { return Marshal.PtrToStringUni(pv.pwszVal) ?? ""; }
        finally { Marshal.FreeCoTaskMem(pv.pwszVal); }
    }
}
'@
}

[PbLnkAumid]::Set($lnk, $AppId)

# --- verify ----------------------------------------------------------------
$actual = ''
for ($attempt = 0; $attempt -lt 5 -and -not $actual; ++$attempt) {
    try { $actual = [PbLnkAumid]::Get($lnk) }
    catch { Start-Sleep -Milliseconds 250 }
}
if (-not $actual) {
    $shell2 = New-Object -ComObject Shell.Application
    $folder = $shell2.Namespace((Split-Path -Parent $lnk))
    $item = $folder.ParseName((Split-Path -Leaf $lnk))
    $actual = $item.ExtendedProperty('System.AppUserModel.ID')
    [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($shell2)
}

Write-Host "Start Menu entry: $lnk"
Write-Host "  target: $targetText"
Write-Host "  icon:   $iconText"
Write-Host "  appid:  $actual  (expected $AppId)"
if ($actual -ne $AppId) {
    Write-Warning "AppUserModelID did not stick; the shortcut will group by target path."
}

Write-Host ""
Write-Host "If a surface (NAI OS dock / app menu) still shows the old VLC entry,"
Write-Host "unpin that old item and pin 'NAI PaintBrush' from the app menu."
