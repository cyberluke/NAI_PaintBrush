# Build and run the Paintbrush Motion Toys PuppetEffect spike harness
# (Windows, msys2 mingw64 Qt6). Compiles the REAL puppet runtime sources —
# no VLC build, no moc (the runtime is plain C++17 + QtCore/QtGui values).
#
#   pwsh _REVIEW/harness/pb_puppet_harness/build_run.ps1
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

$env:PATH = "$qt\bin;" + $env:PATH  # cc1plus and the exe need the mingw DLLs

Write-Host "== compile+link =="
& $gpp -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers @includeArgs `
    (Join-Path $here 'pb_puppet_harness.cpp') `
    (Join-Path $src 'motion\pb_puppet.cpp') `
    -o (Join-Path $build 'pb_puppet_harness.exe') `
    -L "$qt\lib" -lQt6Core -lQt6Gui
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "== run =="
Push-Location $build
& "$build\pb_puppet_harness.exe" -platform offscreen
$code = $LASTEXITCODE
Pop-Location
exit $code