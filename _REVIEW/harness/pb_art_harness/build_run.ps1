# Build and run the Paintbrush 2.0 art/harvest/boost acceptance harness
# (Windows, msys2 mingw64 Qt6). Compiles the REAL paintbrush sources with a
# stub vlc_common.h — no VLC build is involved.
#
#   pwsh _REVIEW/harness/pb_art_harness/build_run.ps1
$ErrorActionPreference = 'Stop'

$qt = 'C:\msys64\mingw64'
$moc = "$qt\share\qt6\bin\moc.exe"
$gpp = "$qt\bin\g++.exe"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = (Resolve-Path (Join-Path $here '..\..\..')).Path
$src = Join-Path $root 'modules\gui\qt\paintbrush'
$build = Join-Path $here 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null

$includeArgs = @(
    '-I', (Join-Path $here 'stub'),
    '-I', $src,
    '-I', "$qt\include\qt6",
    '-I', "$qt\include\qt6\QtCore",
    '-I', "$qt\include\qt6\QtGui"
)

$env:PATH = "$qt\bin;" + $env:PATH  # moc, cc1plus and the exe need the mingw DLLs

Write-Host "== moc =="
foreach ($h in @('pb_document', 'pb_history', 'pb_harvest', 'pb_project_store')) {
    & $moc @includeArgs "$src\$h.hpp" -o "$build\moc_$h.cpp"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$sources = @(
    (Join-Path $here 'pb_art_harness.cpp'),
    (Join-Path $here 'stub_vlc.cpp'),
    "$src\pb_common.cpp", "$src\pb_layer.cpp", "$src\pb_document.cpp",
    "$src\pb_history.cpp", "$src\pb_harvest.cpp", "$src\pb_boost.cpp",
    "$src\pb_project_store.cpp",
    "$src\art\runtime\material_registry.cpp",
    "$src\art\surface\paper_stock.cpp", "$src\art\surface\paper_renderer.cpp",
    "$src\art\stroke\bristle_brush.cpp",
    "$src\art\materials\glitter\glitter_material.cpp",
    "$src\art\finish\effects.cpp",
    "$src\art\style\mood.cpp", "$src\art\style\typography_style.cpp", "$src\art\style\badges.cpp",
    "$build\moc_pb_document.cpp", "$build\moc_pb_history.cpp",
    "$build\moc_pb_harvest.cpp", "$build\moc_pb_project_store.cpp"
)

Write-Host "== compile+link =="
& $gpp -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers @includeArgs `
    @sources -o "$build\pb_art_harness.exe" `
    -L "$qt\lib" -lQt6Core -lQt6Gui
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "== run =="
Push-Location $build
& "$build\pb_art_harness.exe" -platform offscreen
$code = $LASTEXITCODE
Pop-Location
exit $code
