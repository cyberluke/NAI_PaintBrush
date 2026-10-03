# Launch Paintbrush with SAM3 + file logging (run7). Sets env then runs vlc.
$ErrorActionPreference = "Stop"
$root = "D:\_SATIN_AI\NAI-OS\vlc"
$build = Join-Path $root "win64-meson"
$bin = Join-Path $build "bin"
$vlc = Join-Path $bin "vlc.exe"

$env:VLC_PLUGIN_PATH = Join-Path $build "modules"
$env:QT_PLUGIN_PATH  = "C:\msys64\mingw64\share\qt6\plugins"
$env:QML_IMPORT_PATH = "C:\msys64\mingw64\share\qt6\qml"
$env:QML2_IMPORT_PATH = $env:QML_IMPORT_PATH
$mingw = "C:\msys64\mingw64\bin"
$env:PATH = "$bin;$mingw;$(Join-Path $build 'lib');$(Join-Path $build 'src');" + $env:PATH

& $vlc `
  "--paintbrush" `
  "--no-media-library" `
  "--no-qt-privacy-ask" `
  "--no-qt-updates-notif" `
  "--paintbrush-sam-model=D:\_SATIN_AI\models\sam3-q4_0.ggml" `
  "--paintbrush-sam-device=cpu" `
  "--verbose=2" `
  "--file-logging" `
  "--logmode=text" `
  "--logfile=D:\_SATIN_AI\NAI-OS\vlc\pb_run7.log" `
  "--log-verbose=2" `
  "-I" "qt"