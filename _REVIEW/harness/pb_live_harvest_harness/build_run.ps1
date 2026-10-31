# Build and run the Paintbrush Live Harvest correctness harness (Windows,
# msys2 mingw64 Qt6). Compiles the PURE-Qt Live Harvest logic
# (pb_selection.hpp + pb_media_logic.hpp + pb_common.hpp helpers) against Qt6
# — no VLC, no Q_OBJECT, no moc, no stub needed.
#
#   pwsh _REVIEW/harness/pb_live_harvest_harness/build_run.ps1
$ErrorActionPreference = 'Stop'

$qt = 'C:\msys64\mingw64'
$gpp = "$qt\bin\g++.exe"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = (Resolve-Path (Join-Path $here '..\..\..')).Path
$src = Join-Path $root 'modules\gui\qt\paintbrush'
$build = Join-Path $here 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null

$includeArgs = @(
    '-I', $src,
    '-I', "$qt\include\qt6",
    '-I', "$qt\include\qt6\QtCore",
    '-I', "$qt\include\qt6\QtGui"
)
$env:PATH = "$qt\bin;" + $env:PATH

Write-Host "== compile+link =="
& $gpp -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers @includeArgs `
    (Join-Path $here 'pb_live_harvest_harness.cpp') `
    -o "$build\pb_live_harvest_harness.exe" `
    -L "$qt\lib" -lQt6Core -lQt6Gui
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "== run =="
Push-Location $build
& "$build\pb_live_harvest_harness.exe" -platform offscreen
exit $LASTEXITCODE