# Isolated-worktree launcher for the Holographic Materials branch.
#
# The shared launcher hardcodes D:\_SATIN_AI\NAI-OS\vlc, which makes a "worktree"
# run silently use the main tree's binary, plugin and log. This script derives
# every path from its own location so an isolated build cannot touch the live
# session.
#
#   pwsh scripts/launch-pb-noskip-holo.ps1
#   pwsh scripts/launch-pb-noskip-holo.ps1 -CreativeSelfTest

[CmdletBinding()]
param(
    [string[]]$Media = @(),
    [switch]$MatteSelfTest,
    [switch]$CreativeSelfTest,
    [string]$SamModel = "",
    [string]$SamDevice = "",
    [string]$LogFile = "",
    [switch]$NoAutosave
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$bin = Join-Path $root "win64-meson\bin"
$exe = Join-Path $bin "nai-paintbrush.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "NAI Paintbrush build output not found: $exe`nBuild it first: ninja -C win64-meson bin/nai-paintbrush.exe"
}
if ($LogFile -eq "") {
    $LogFile = Join-Path $root "pb_noskip_holo.log"
}

$env:VLC_PLUGIN_PATH = Join-Path (Join-Path $root "win64-meson") "modules"
$env:QT_PLUGIN_PATH = "C:\msys64\mingw64\share\qt6\plugins"
$env:QML_IMPORT_PATH = "C:\msys64\mingw64\share\qt6\qml"
$env:QML2_IMPORT_PATH = $env:QML_IMPORT_PATH
$env:PATH = "$bin;C:\msys64\mingw64\bin;$(Join-Path $root 'win64-meson\lib');$(Join-Path $root 'win64-meson\src');" + $env:PATH

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
if ($CreativeSelfTest) { $extra += "--paintbrush-creative-selftest" }
foreach ($item in $Media) { if ($item -ne "") { $extra += "--paintbrush-open=$item" } }
if ($NoAutosave) { $extra += "--no-paintbrush-autosave" }
if ($SamModel -ne "") { $extra += "--paintbrush-sam-model=$SamModel" }
if ($SamDevice -ne "") { $extra += "--paintbrush-sam-device=$SamDevice" }

& $exe `
  "--plugin-path" "$modules" `
  "--paintbrush" "--no-media-library" "--no-qt-privacy-ask" "--no-qt-updates-notif" `
  "--no-qt-name-in-title" "--no-one-instance" `
  "--no-avcodec-hurry-up" `
  "--avcodec-skip-frame=-1" `
  "--avcodec-skip-idct=-1" `
  "--avcodec-skiploopfilter=-1" `
  "--verbose=2" "--file-logging" "--logmode=text" "--logfile=$LogFile" "--log-verbose=2" `
  "-I" "qt" @extra
