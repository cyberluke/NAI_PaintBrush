# A/B test: no decoder catch-up skipping (hurry-up disabled, no skip frame/idct/loopfilter)
# Product identity: the app runs as paintbrush.exe (a copy of the vlc.exe
# binary) so the process/taskbar never clash with a media player.
#
# Optional arguments (defaults preserve the original no-skip launch):
#   -Media <path>        open a video in the Paintbrush workspace
#   -MatteSelfTest       run the end-to-end neural matte acceptance self-test
#   -SamModel <path>     SAM3 model for object selection
#   -SamDevice <auto|cpu|gpu>
#
# Examples:
#   .\scripts\launch-pb-noskip.ps1
#   .\scripts\launch-pb-noskip.ps1 -Media D:\clips\dance.mp4 -MatteSelfTest
[CmdletBinding()]
param(
    [string[]]$Media = @(),
    [switch]$MatteSelfTest,
    [string]$SamModel = "",
    [string]$SamDevice = "",
    [string]$LogFile = "D:\_SATIN_AI\NAI-OS\vlc\pb_noskip.log",
    [switch]$NoAutosave
)

$ErrorActionPreference = "Stop"
$root = "D:\_SATIN_AI\NAI-OS\vlc"
$bin = Join-Path $root "win64-meson\bin"
$vlc = Join-Path $bin "paintbrush.exe"
if (-not (Test-Path -LiteralPath $vlc)) {
    Copy-Item -LiteralPath (Join-Path $bin "vlc.exe") -Destination $vlc -Force
}
$env:VLC_PLUGIN_PATH = Join-Path (Join-Path $root "win64-meson") "modules"
$env:QT_PLUGIN_PATH  = "C:\msys64\mingw64\share\qt6\plugins"
$env:QML_IMPORT_PATH = "C:\msys64\mingw64\share\qt6\qml"
$env:QML2_IMPORT_PATH = $env:QML_IMPORT_PATH
$env:PATH = "$bin;C:\msys64\mingw64\bin;$(Join-Path $root 'win64-meson\lib');$(Join-Path $root 'win64-meson\src');" + $env:PATH

$extra = @()
if ($MatteSelfTest) { $extra += "--paintbrush-matte-selftest" }
foreach ($item in $Media) { if ($item -ne "") { $extra += "--paintbrush-open=$item" } }
if ($NoAutosave) { $extra += "--no-paintbrush-autosave" }
if ($SamModel -ne "") { $extra += "--paintbrush-sam-model=$SamModel" }
if ($SamDevice -ne "") { $extra += "--paintbrush-sam-device=$SamDevice" }

& $vlc `
  "--paintbrush" "--no-media-library" "--no-qt-privacy-ask" "--no-qt-updates-notif" `
  "--no-qt-name-in-title" "--no-one-instance" `
  "--no-avcodec-hurry-up" `
  "--avcodec-skip-frame=-1" `
  "--avcodec-skip-idct=-1" `
  "--avcodec-skiploopfilter=-1" `
  "--verbose=2" "--file-logging" "--logmode=text" "--logfile=$LogFile" "--log-verbose=2" `
  "-I" "qt" @extra
