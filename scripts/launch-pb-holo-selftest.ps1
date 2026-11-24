# Test-only launcher for the isolated Holographic Materials worktree.
#
# Runs the worktree nai-paintbrush.exe + worktree Qt plugin with the creative
# self-test. It never touches D:\_SATIN_AI\NAI-OS\vlc (the live tree).
#
# Why this exists: VLC's Windows plug-in loader uses
#   LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_APPLICATION_DIR
#                       | LOAD_LIBRARY_SEARCH_SYSTEM32)
# so plug-in dependencies (Qt6*.dll, libstdc++-6.dll, ...) must sit next to
# nai-paintbrush.exe. PATH alone is NOT searched for plug-in dependencies.
# The worktree win64-meson\bin therefore needs the same runtime deployment as
# the main bin (Qt6*.dll + the mingw/glib closure from C:\msys64\mingw64\bin).
#
# --plugin-path is not a valid VLC 4 option; VLC_PLUGIN_PATH is the mechanism.
#
#   pwsh scripts/launch-pb-holo-selftest.ps1
#   pwsh scripts/launch-pb-holo-selftest.ps1 -Media D:\clips\dance.mp4

[CmdletBinding()]
param(
    [string[]]$Media = @(),
    [string]$LogFile = (Join-Path $env:TEMP "pb-holo-selftest.log"),
    [switch]$ConsoleLog,
    [switch]$PlainInterface,
    [switch]$QtDebug
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$bin = Join-Path $root "win64-meson\bin"
$modules = Join-Path $root "win64-meson\modules"
$exe = Join-Path $bin "nai-paintbrush.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "NAI Paintbrush worktree build output not found: $exe"
}

$env:VLC_PLUGIN_PATH = $modules
$env:QT_PLUGIN_PATH = "C:\msys64\mingw64\share\qt6\plugins"
$env:QML_IMPORT_PATH = "C:\msys64\mingw64\share\qt6\qml"
$env:QML2_IMPORT_PATH = $env:QML_IMPORT_PATH
$env:PATH = "$bin;C:\msys64\mingw64\bin;$(Join-Path $root 'win64-meson\lib');$(Join-Path $root 'win64-meson\src');" + $env:PATH
if ($QtDebug) {
    $env:QT_DEBUG_PLUGINS = "1"
    $env:QT_LOGGING_RULES = "*=true"
}

$extra = @()
foreach ($item in $Media) { if ($item -ne "") { $extra += "--paintbrush-open=$item" } }
if ($ConsoleLog) {
    $extra += @("--verbose=3", "--no-file-logging")
} else {
    $extra += @("--verbose=2", "--file-logging", "--logmode=text",
                "--logfile=$LogFile", "--log-verbose=2")
}

$modeArgs = @("--paintbrush", "--paintbrush-creative-selftest")
if ($PlainInterface) { $modeArgs = @() }

& $exe @modeArgs `
  "--no-one-instance" "-I" "qt" `
  "--no-media-library" "--no-qt-privacy-ask" "--no-qt-updates-notif" `
  "--no-qt-name-in-title" @extra
