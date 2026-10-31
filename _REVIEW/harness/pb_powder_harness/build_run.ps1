# Build and run the Paintbrush glitter-powder correctness harness (Windows,
# msys2 mingw64 Qt6). Compiles the PURE-Qt powder engine
# (glitter_powder.cpp) directly against Qt6 — no VLC, no moc.
#
#   pwsh _REVIEW/harness/pb_powder_harness/build_run.ps1
$ErrorActionPreference = 'Stop'

$qt = 'C:\msys64\mingw64'
$gpp = "$qt\bin\g++.exe"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = (Resolve-Path (Join-Path $here '..\..\..')).Path
$build = Join-Path $here 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null

$includeArgs = @(
    '-I', $root,
    '-I', "$qt\include\qt6",
    '-I', "$qt\include\qt6\QtCore",
    '-I', "$qt\include\qt6\QtGui"
)
$env:PATH = "$qt\bin;" + $env:PATH

Write-Host "== compile+link =="
& $gpp -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers @includeArgs `
    (Join-Path $here 'pb_powder_harness.cpp') `
    -o "$build\pb_powder_harness.exe" `
    -L "$qt\lib" -lQt6Core -lQt6Gui
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "== run =="
Push-Location $build
& "$build\pb_powder_harness.exe" -platform offscreen
exit $LASTEXITCODE