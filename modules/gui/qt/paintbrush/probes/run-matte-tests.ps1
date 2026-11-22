<#
.SYNOPSIS
  Build and run the standalone matte-core behaviour harness.

.DESCRIPTION
  Compiles modules/gui/qt/paintbrush/tests/matte_core_test.cpp against the
  real pb_matte.cpp using MSYS2 Qt6 (Core+Gui only, no VLC) and runs the
  assertions. This is the deterministic-stages evidence gate: topology,
  trimap, fractional alpha and foreground decontamination are exercised on
  synthetic fixtures with known ground truth.

.EXAMPLE
  pwsh -File modules/gui/qt/paintbrush/probes/run-matte-tests.ps1
#>
[CmdletBinding()]
param(
    [string]$QtRoot = "C:\msys64\mingw64",
    [string]$BuildDir = "win64-meson\matte-tests"
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))))
$paintbrush = Join-Path $repo "modules\gui\qt\paintbrush"
$source = Join-Path $paintbrush "tests\matte_core_test.cpp"
$core = Join-Path $paintbrush "pb_matte.cpp"
$outDir = Join-Path $repo $BuildDir
$exe = Join-Path $outDir "matte_core_test.exe"

if (-not (Test-Path -LiteralPath (Join-Path $QtRoot "bin\g++.exe"))) {
    throw "g++ not found under $QtRoot\bin"
}

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$env:PATH = (Join-Path $QtRoot "bin") + ";" + (Join-Path $QtRoot "..\usr\bin") + ";" + $env:PATH
$env:PKG_CONFIG_PATH = "/mingw64/lib/pkgconfig"

$pkgConfig = Join-Path $QtRoot "bin\pkg-config.exe"
if (-not (Test-Path -LiteralPath $pkgConfig)) {
    $pkgConfig = "C:\msys64\usr\bin\pkg-config.exe"
}
$cflags = (& $pkgConfig --cflags Qt6Gui).Trim()
$libs = (& $pkgConfig --libs Qt6Gui).Trim()

Write-Host "compiling matte core harness"
$compile = @(
    "-std=c++17", "-O2", "-g", "-Wall", "-Wextra",
    "-fPIC",
    "-I", $paintbrush,
    $source, $core,
    "-o", $exe
) + ($cflags -split " ") + ($libs -split " ")
& (Join-Path $QtRoot "bin\g++.exe") @compile
if ($LASTEXITCODE -ne 0) { throw "matte core harness compilation failed" }

Write-Host "running $exe"
& $exe
$code = $LASTEXITCODE
Write-Output "EXIT=$code"
exit $code
