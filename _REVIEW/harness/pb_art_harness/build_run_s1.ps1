# Regression: rebuild + run the preserved Stream-1 core harness
# (_REVIEW/harness/pb_s1_harness.cpp — document/renderer/typed undo/export/
# project store) against the CURRENT paintbrush sources, with the same stub
# and flags as the Paintbrush 2.0 art harness.
$ErrorActionPreference = 'Stop'
$qt = 'C:\msys64\mingw64'
$moc = "$qt\share\qt6\bin\moc.exe"
$gpp = "$qt\bin\g++.exe"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = (Resolve-Path (Join-Path $here '..\..\..')).Path
$src = Join-Path $root 'modules\gui\qt\paintbrush'
$build = Join-Path $here 'build_s1'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$includeArgs = @('-I', (Join-Path $here 'stub'), '-I', $src, '-I', "$qt\include\qt6",
                 '-I', "$qt\include\qt6\QtCore", '-I', "$qt\include\qt6\QtGui")
$env:PATH = "$qt\bin;" + $env:PATH
foreach ($h in @('pb_document', 'pb_history', 'pb_harvest', 'pb_project_store', 'pb_export')) {
    & $moc @includeArgs "$src\$h.hpp" -o "$build\moc_$h.cpp"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
$sources = @(
    (Join-Path $root '_REVIEW\harness\pb_s1_harness.cpp'),
    (Join-Path $here 'stub_vlc.cpp'),  # var_Inherit*/vlc_object_vaLog; the S1 file adds the rest
    "$src\pb_common.cpp", "$src\pb_layer.cpp", "$src\pb_document.cpp", "$src\pb_history.cpp",
    "$src\pb_harvest.cpp", "$src\pb_project_store.cpp", "$src\pb_export.cpp",
    "$src\art\runtime\material_registry.cpp", "$src\art\surface\paper_stock.cpp",
    "$src\art\surface\paper_renderer.cpp", "$src\art\stroke\bristle_brush.cpp",
    "$src\art\materials\glitter\glitter_material.cpp", "$src\art\finish\effects.cpp",
    "$src\art\style\mood.cpp", "$src\art\style\typography_style.cpp", "$src\art\style\badges.cpp",
    "$build\moc_pb_document.cpp", "$build\moc_pb_history.cpp", "$build\moc_pb_harvest.cpp",
    "$build\moc_pb_project_store.cpp", "$build\moc_pb_export.cpp"
)
& $gpp -std=c++17 -O2 -Wall -Wno-missing-field-initializers @includeArgs @sources -o "$build\pb_s1.exe" -L "$qt\lib" -lQt6Core -lQt6Gui
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Push-Location $build
& "$build\pb_s1.exe" -platform offscreen
$code = $LASTEXITCODE
Pop-Location
exit $code
