<#
.SYNOPSIS
  Launch NAI Paintbrush (VLC --paintbrush) with the correct runtime environment.

.DESCRIPTION
  Sets the required environment variables (plugin/QML paths and PATH), then
  launches the built nai-paintbrush.exe in Paintbrush mode. Fails clearly if the build
  output is missing.

  Optional parameters:
    -Media <path>   an optional media/video file to open after launch
    -Demo           use the Phase 3 release/demo profile (-Demo profile; see
                    -ReleaseProfile and the --paintbrush-demo-mode flag)
    -Extra <args>   additional VLC argument string passed through verbatim
    -SamModel <p>   optional SAM3 model override; OMIT to use the saved
                    NAI Settings value (paintbrush-sam-model in vlcrc)
    -SamDevice <d>  optional SAM3 device override: auto/gpu/cpu; OMIT to use
                    the saved NAI Settings value (paintbrush-sam-device)
    -NoSam          don't pass the SAM model/device args (e.g. model missing)
    -NoLaunch       only set the environment and print the command (no run)
    -VerboseLog     pass --verbose=2 -I qt

.NOTES
  NAI Settings persists into VLC's config file (vlcrc). A command-line
  --paintbrush-sam-device/--paintbrush-sam-model overrides that file for the
  whole run, so the defaults here are EMPTY on purpose: a normal restart must
  honour what the user chose in NAI Settings. Pass -SamDevice/-SamModel only
  for a deliberate one-off override.

.EXAMPLE
  .\scripts\run-paintbrush.ps1                       # uses saved NAI Settings
  .\scripts\run-paintbrush.ps1 -Media "D:\clip.mp4"
  .\scripts\run-paintbrush.ps1 -Demo                 # release/demo profile
  .\scripts\run-paintbrush.ps1 -Extra "--no-video-title-show"
  .\scripts\run-paintbrush.ps1 -SamDevice cpu        # one-off CPU override
#>
[CmdletBinding()]
param(
    [string]$Media,
    [switch]$Demo,
    [string]$SamModel = "",
    [string]$SamDevice = "",
    [switch]$NoSam,
    [switch]$NoLaunch,
    [switch]$VerboseLog,
    [string]$Extra
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "win64-meson"
$bin   = Join-Path $build "bin"
$exe   = Join-Path $bin "nai-paintbrush.exe"

# --- fail clearly if the build output is missing -------------------------
if (-not (Test-Path -LiteralPath $exe)) {
    Write-Error "Paintbrush build output not found: $exe`nRun a build first (see modules/gui/qt/paintbrush/BUILDING.md)."
    exit 1
}

# --- environment ----------------------------------------------------------
$env:VLC_PLUGIN_PATH = Join-Path $build "modules"
$env:QT_PLUGIN_PATH  = "C:\msys64\mingw64\share\qt6\plugins"
$env:QML_IMPORT_PATH = "C:\msys64\mingw64\share\qt6\qml"
$env:QML2_IMPORT_PATH = $env:QML_IMPORT_PATH
$mingw = "C:\msys64\mingw64\bin"
$env:PATH = "$bin;$mingw;$(Join-Path $build 'lib');$(Join-Path $build 'src');" + $env:PATH

# --- arguments ------------------------------------------------------------
$args = @("--paintbrush", "--no-media-library", "--no-qt-privacy-ask", "--no-qt-updates-notif")
if (-not $NoSam) {
    # Only pass an explicit override. With no flag VLC reads the persistent
    # NAI Settings value from vlcrc; pinning "cpu" here made every restart
    # ignore the user's GPU choice.
    if ($SamModel) {
        $args += "--paintbrush-sam-model=$SamModel"
    }
    if ($SamDevice) {
        $args += "--paintbrush-sam-device=$SamDevice"
    }
}
if ($VerboseLog) {
    $args += "--verbose=2"
    $args += "-I"
    $args += "qt"
}
if ($Demo) {
    $args += "--paintbrush-demo-mode"
}
if ($Media) {
    $args += "--input"
    $args += $Media
}
if ($Extra) {
    # split on whitespace, preserving quoted groups is not needed here
    $args += ($Extra -split "\s+")
}

$cmd = "& '$exe' " + (($args | ForEach-Object { if ($_ -match "\s") { "'$_'" } else { $_ } }) -join " ")
Write-Host "Launcher root: $root"
Write-Host "NAI Paintbrush: $exe"
Write-Host "Command: $cmd"

if (-not $NoLaunch) {
    Write-Host "Launching NAI Paintbrush..."
    & $exe @args
}