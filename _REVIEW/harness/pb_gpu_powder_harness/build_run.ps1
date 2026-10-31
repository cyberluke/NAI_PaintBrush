# Build and run the NAI Paintbrush GPU glitter-powder acceptance harness
# (Windows, msys2 mingw64 Qt6). Compiles the engine sources directly into ONE
# translation unit (pb_gpu_runtime.cpp + glitter_powder.cpp + effects.cpp) —
# no VLC, no moc. Runs the executable with `-platform offscreen` so the QRhi
# D3D11 backend renders offscreen (falls back to GLES2 when D3D11 is absent).
#
#   pwsh _REVIEW/harness/pb_gpu_powder_harness/build_run.ps1
$ErrorActionPreference = 'Stop'

$qt = 'C:\msys64\mingw64'
$gpp = "$qt\bin\g++.exe"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = (Resolve-Path (Join-Path $here '..\..\..')).Path
$build = Join-Path $here 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null

# Qt versioned subdir under QtGui (the RHI headers live at
#   <qt>\include\qt6\QtGui\<ver>\QtGui\rhi\qshader.h
# so <rhi/qshader.h> / <rhi/qrhi.h> resolve only with -I <ver>/QtGui).
$qtGuiVer = (Get-ChildItem -Path "$qt\include\qt6\QtGui" -Directory |
                 Where-Object { $_.Name -match '^6\.' } | Select-Object -First 1).Name
$qtGuiNested = "$qt\include\qt6\QtGui\$qtGuiVer\QtGui"
Write-Host "== QtGui versioned include: $qtGuiNested =="
if (-not (Test-Path "$qtGuiNested\rhi")) { throw "rhi include dir missing under $qtGuiNested" }

$includeArgs = @(
    '-I', $root,
    '-I', "$qt\include\qt6",
    '-I', "$qt\include\qt6\QtCore",
    '-I', "$qt\include\qt6\QtGui",
    '-I', $qtGuiNested
)
$env:PATH = "$qt\bin;" + $env:PATH

Write-Host "== compile+link =="
& $gpp -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers @includeArgs `
    (Join-Path $here 'pb_gpu_powder_harness.cpp') `
    -o "$build\pb_gpu_powder_harness.exe" `
    -L "$qt\lib" -lQt6Core -lQt6Gui
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "== run =="
Push-Location $build
& "$build\pb_gpu_powder_harness.exe" -platform offscreen "$build"
$code = $LASTEXITCODE
Pop-Location
Write-Host "== exit: $code =="
exit $code