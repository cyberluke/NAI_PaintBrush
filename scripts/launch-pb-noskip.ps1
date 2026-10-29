# A/B test: no decoder catch-up skipping (hurry-up disabled, no skip frame/idct/loopfilter)
# Product identity: the app runs as paintbrush.exe (a copy of the vlc.exe
# binary) so the process/taskbar never clash with a media player.
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

& $vlc `
  "--paintbrush" "--no-media-library" "--no-qt-privacy-ask" "--no-qt-updates-notif" `
  "--no-qt-name-in-title" `
  "--paintbrush-sam-model=D:\_SATIN_AI\models\sam3-q4_0.ggml" "--paintbrush-sam-device=cpu" `
  "--no-avcodec-hurry-up" `
  "--avcodec-skip-frame=-1" `
  "--avcodec-skip-idct=-1" `
  "--avcodec-skiploopfilter=-1" `
  "--verbose=2" "--file-logging" "--logmode=text" "--logfile=D:\_SATIN_AI\NAI-OS\vlc\pb_noskip.log" "--log-verbose=2" `
  "-I" "qt"