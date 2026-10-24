# Build and run the Paintbrush Showroom LIVE acceptance harness.
# Real video decode (libvlc vmem) + real SAM3 segmentation -> Keep A/B/C ->
# Canvas -> Boost (bridge pose from a real mid-gap frame) -> 4K export.
#
#   pwsh _REVIEW/harness/pb_live_harness/build_run.ps1 [video] [model] [outdir]
$ErrorActionPreference = 'Stop'

$qt = 'C:\msys64\mingw64'
$moc = "$qt\share\qt6\bin\moc.exe"
$gpp = "$qt\bin\g++.exe"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = (Resolve-Path (Join-Path $here '..\..\..')).Path
$src = Join-Path $root 'modules\gui\qt\paintbrush'
$build = Join-Path $here 'build'
$outdir = Join-Path $build 'live'
New-Item -ItemType Directory -Force -Path $build, $outdir | Out-Null

$video = if ($args.Count -ge 1) { $args[0] } else { 'C:\Users\lukes.COREI9\Downloads\NewJeans 뉴진스 Hype Boy Dance Practice.mkv' }
$model = if ($args.Count -ge 2) { $args[1] } else { 'D:\_SATIN_AI\models\sam3-q4_0.ggml' }
if (-not (Test-Path -LiteralPath $video)) { throw "video not found: $video" }
if (-not (Test-Path -LiteralPath $model)) { throw "model not found: $model" }

# argv through a native exe is ANSI-lossy for non-ASCII names; stage a
# copy with an ASCII name so libvlc can open it reliably.
$videoArg = $video
if ($video -match '[^\x00-\x7F]') {
    $videoArg = Join-Path $build 'video_input.mkv'
    Copy-Item -LiteralPath $video -Destination $videoArg -Force
    Write-Host "staged video to ASCII path: $videoArg"
}

$includeArgs = @(
    '-I', (Join-Path $here '..\pb_art_harness\stub'),
    '-I', $src,
    '-I', "$qt\include\qt6",
    '-I', "$qt\include\qt6\QtCore",
    '-I', "$qt\include\qt6\QtGui",
    '-I', "$qt\include\qt6\QtGui\6.11.2\QtGui",
    '-I', (Join-Path $root 'include'),          # real vlc/libvlc.h
    '-I', (Join-Path $root 'contrib\x86_64-w64-mingw32\include')
)

$env:PATH = "$qt\bin;" + $env:PATH

Write-Host "== moc =="
foreach ($h in @('pb_document', 'pb_history', 'pb_harvest', 'pb_project_store')) {
    & $moc @includeArgs "$src\$h.hpp" -o "$build\moc_$h.cpp"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$sources = @(
    (Join-Path $here 'pb_live_harness.cpp'),
    (Join-Path $here '..\pb_art_harness\stub_vlc.cpp'),
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
    @sources -o "$build\pb_live_harness.exe" `
    -L "$qt\lib" -lQt6Core -lQt6Gui `
    -L (Join-Path $root 'win64-meson\lib') -lvlc `
    -L (Join-Path $root 'contrib\x86_64-w64-mingw32\lib') `
    -lsam3 -lggml -lggml-base -lggml-cpu -lstdc++
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "== run =="
# runtime env: libvlc.dll (win64-meson\lib) + libvlccore (src) + Qt + plugins
$root = (Resolve-Path (Join-Path $here '..\..\..')).Path
$env:PATH = "$qt\bin;$root;$(Join-Path $root 'win64-meson');$(Join-Path $root 'win64-meson\lib');$(Join-Path $root 'win64-meson\src');$(Join-Path $root 'win64-meson\bin');" + $env:PATH
$env:VLC_PLUGIN_PATH = Join-Path $root 'win64-meson\modules'
Push-Location $build
$env:PB_LIVE_OUT = $outdir
& "$build\pb_live_harness.exe" $videoArg $model $outdir
$code = $LASTEXITCODE
Pop-Location
exit $code