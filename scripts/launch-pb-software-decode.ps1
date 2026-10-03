# Launch Paintbrush with SOFTWARE video decode (avcodec-hw=none) for decode-artifact test.
$ErrorActionPreference = "Stop"
$root = "D:\_SATIN_AI\NAI-OS\vlc"
$bin = Join-Path $root "win64-meson\bin"
$vlc = Join-Path $bin "vlc.exe"
$env:VLC_PLUGIN_PATH = Join-Path (Join-Path $root "win64-meson") "modules"
$env:QT_PLUGIN_PATH  = "C:\msys64\mingw64\share\qt6\plugins"
$env:QML_IMPORT_PATH = "C:\msys64\mingw64\share\qt6\qml"
$env:QML2_IMPORT_PATH = $env:QML_IMPORT_PATH
$env:PATH = "$bin;C:\msys64\mingw64\bin;$(Join-Path $root 'win64-meson\lib');$(Join-Path $root 'win64-meson\src');" + $env:PATH

& $vlc `
  "--paintbrush" "--no-media-library" "--no-qt-privacy-ask" "--no-qt-updates-notif" `
  "--paintbrush-sam-model=D:\_SATIN_AI\models\sam3-q4_0.ggml" "--paintbrush-sam-device=cpu" `
  "--avcodec-hw=none" `
  "--verbose=2" "--file-logging" "--logmode=text" "--logfile=D:\_SATIN_AI\NAI-OS\vlc\pb_sw.log" "--log-verbose=2" `
  "-I" "qt"