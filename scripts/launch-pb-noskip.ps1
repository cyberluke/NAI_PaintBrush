# A/B test: no decoder catch-up skipping (hurry-up disabled, no skip frame/idct/loopfilter)
# Product identity: the build produces nai-paintbrush.exe (the VLC engine
# binary renamed at link time), so the process/taskbar never clash with a
# media player.
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
$exe = Join-Path $bin "nai-paintbrush.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "NAI Paintbrush build output not found: $exe`nBuild it first: ninja -C win64-meson bin/nai-paintbrush.exe"
}
$env:VLC_PLUGIN_PATH = Join-Path (Join-Path $root "win64-meson") "modules"
$env:QT_PLUGIN_PATH  = "C:\msys64\mingw64\share\qt6\plugins"
$env:QML_IMPORT_PATH = "C:\msys64\mingw64\share\qt6\qml"
$env:QML2_IMPORT_PATH = $env:QML_IMPORT_PATH
$env:PATH = "$bin;C:\msys64\mingw64\bin;$(Join-Path $root 'win64-meson\lib');$(Join-Path $root 'win64-meson\src');" + $env:PATH

# A rebuilt (or re-signed) libqt_plugin.dll invalidates modules\plugins.dat and
# VLC then silently runs without the Qt interface (no window). Refresh the cache
# whenever the plugin is newer than the cache.
$modules = Join-Path $root 'win64-meson\modules'
$pluginsDat = Join-Path $modules 'plugins.dat'
$qtPlugin = Join-Path $modules 'libqt_plugin.dll'
if ((Test-Path -LiteralPath $qtPlugin) -and
    ((-not (Test-Path -LiteralPath $pluginsDat)) -or
     ((Get-Item -LiteralPath $qtPlugin).LastWriteTime -gt (Get-Item -LiteralPath $pluginsDat).LastWriteTime))) {
    & (Join-Path $bin 'vlc-cache-gen.exe') $modules | Out-Null
}

$extra = @()
if ($MatteSelfTest) { $extra += "--paintbrush-matte-selftest" }
foreach ($item in $Media) { if ($item -ne "") { $extra += "--paintbrush-open=$item" } }
if ($NoAutosave) { $extra += "--no-paintbrush-autosave" }
if ($SamModel -ne "") { $extra += "--paintbrush-sam-model=$SamModel" }
if ($SamDevice -ne "") { $extra += "--paintbrush-sam-device=$SamDevice" }

& $exe `
  "--paintbrush" "--no-media-library" "--no-qt-privacy-ask" "--no-qt-updates-notif" `
  "--no-qt-name-in-title" "--no-one-instance" `
  "--no-avcodec-hurry-up" `
  "--avcodec-skip-frame=-1" `
  "--avcodec-skip-idct=-1" `
  "--avcodec-skiploopfilter=-1" `
  "--verbose=2" "--file-logging" "--logmode=text" "--logfile=$LogFile" "--log-verbose=2" `
  "-I" "qt" @extra
