# Test-only launcher for the isolated Holographic Materials worktree.
#
# Runs the worktree nai-paintbrush.exe + worktree Qt plugin with the creative
# self-test. It never touches D:\_SATIN_AI\NAI-OS\vlc (the live tree).
# It also starts with a fresh Roaming APPDATA under %TEMP%, so the test cannot
# wait on or overwrite a live Paintbrush recovery/config document.
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
    [string]$DataRoot = "",
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

# A live Paintbrush session holds the user's recovery/config lock. The app
# enables Qt test paths when --paintbrush-creative-selftest is set; this script
# also points APPDATA at a temp Roaming tree and uses a temp vlcrc with the VLC
# singleton disabled. LOCALAPPDATA is left alone so the packaged parser
# fallback still works.
if ([string]::IsNullOrWhiteSpace($DataRoot)) {
    $DataRoot = Join-Path $env:TEMP ("pb-holo-selftest-data-" + [guid]::NewGuid().ToString("N"))
}
$roaming = Join-Path $DataRoot "Roaming"
New-Item -ItemType Directory -Path $roaming -Force | Out-Null
$env:APPDATA = $roaming
Write-Output "isolated APPDATA=$env:APPDATA"

# The live VLC config can set one-instance=1; a second VLC then waits on the
# first instance's global IPC before the Qt interface starts. Pin the test to
# its own config with the singleton disabled.
$vlcConfig = Join-Path $DataRoot "vlcrc"
@("[core]", "one-instance=0", "one-instance-when-started-from-file=0") |
    Set-Content -LiteralPath $vlcConfig -Encoding ascii

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
  "--config=$vlcConfig" "--no-one-instance" "-I" "qt" `
  "--no-media-library" "--no-qt-privacy-ask" "--no-qt-updates-notif" `
  "--no-qt-name-in-title" @extra
