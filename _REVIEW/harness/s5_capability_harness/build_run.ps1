# Build and run the Stream-5 acceptance harness (Windows, msys2 mingw64 Qt6).
#
# Requires: C:\msys64\mingw64 with Qt6 dev packages (qt6-base, qt6-network).
# The harness compiles the REAL paintbrush service sources with a stub
# vlc_common.h — no VLC build is involved.
$ErrorActionPreference = 'Stop'

$qt = 'C:\msys64\mingw64'
$moc = "$qt\share\qt6\bin\moc.exe"
$gpp = "$qt\bin\g++.exe"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here
$src = (Resolve-Path (Join-Path $here '..\..\..\modules\gui\qt\paintbrush')).Path
$includeArgs = @(
    # the shared stub (pb_art_harness/stub) tracks pb_common.hpp's VLC usage
    '-I', (Resolve-Path (Join-Path $here '..\pb_art_harness\stub')).Path,
    '-I', $src,
    '-I', "$qt\include\qt6",
    '-I', "$qt\include\qt6\QtCore",
    '-I', "$qt\include\qt6\QtGui",
    '-I', "$qt\include\qt6\QtNetwork"
)

$env:PATH = "$qt\bin;" + $env:PATH  # moc and the test exe need Qt DLLs

Write-Host "== moc =="
& $moc @includeArgs "$src\pb_generation_service.hpp" -o moc_generation.cpp
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $moc @includeArgs "$src\pb_assistant_service.hpp" -o moc_assistant.cpp
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $moc @includeArgs "$src\pb_lookup_service.hpp" -o moc_lookup.cpp
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "== compile+link =="
& $gpp -std=c++17 -O2 -Wall -Wextra @includeArgs `
    s5_acceptance.cpp `
    "$src\pb_capability.cpp" `
    "$src\pb_generation_service.cpp" `
    "$src\pb_assistant_service.cpp" `
    "$src\pb_lookup_service.cpp" `
    moc_generation.cpp moc_assistant.cpp moc_lookup.cpp `
    -o s5_acceptance.exe `
    -L "$qt\lib" -lQt6Core -lQt6Gui -lQt6Network
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "== run =="
& .\s5_acceptance.exe
exit $LASTEXITCODE