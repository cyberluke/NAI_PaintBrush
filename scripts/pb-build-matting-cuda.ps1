<#
.SYNOPSIS
  Build pb_matting_cuda.dll (MSVC + ONNX Runtime CUDA) and package it.

.DESCRIPTION
  One command for the native neural matting component:

    1. configure CMake with the pinned ONNX Runtime package;
    2. build Release x64 with the installed MSVC toolchain;
    3. package the DLL closure + ONNX graphs into <repo>\win64-meson\bin\matting.

  Defaults point at the verified local dependency cache under
  D:\_SATIN_AI\NAI-OS\gpu-matting. Override the paths for another machine; the
  versions are pinned in the packaging manifest (DEPENDENCIES.json).

.EXAMPLE
  pwsh -File scripts/pb-build-matting-cuda.ps1 -Build
  pwsh -File scripts/pb-build-matting-cuda.ps1 -ConfigureOnly
#>
[CmdletBinding()]
param(
    [string]$OrtRoot = "D:\_SATIN_AI\NAI-OS\gpu-matting\deps\ort-1.30.0",
    [string]$CudnnDir = "D:\_SATIN_AI\NAI-OS\gpu-matting\deps\cudnn-9.28.1.4",
    [string]$CudaBin = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3\bin\x64",
    [string]$ModelDir = "D:\_SATIN_AI\NAI-OS\gpu-matting\models\vitmatte",
    [string]$BuildDir = "D:\_SATIN_AI\NAI-OS\gpu-matting\build-msvc",
    [string]$Dest = "",
    [switch]$ConfigureOnly,
    [switch]$SkipPackage
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$source = Join-Path $repo "modules\gui\qt\paintbrush\gpu\matting"

if (-not (Test-Path -LiteralPath $OrtRoot)) { throw "ORT_ROOT missing: $OrtRoot" }
if (-not (Test-Path -LiteralPath (Join-Path $source "CMakeLists.txt"))) {
    throw "matting source missing: $source"
}

$cmake = "cmake"
Write-Host "configure: $source"
& $cmake -S $source -B $BuildDir -G "Visual Studio 17 2022" -A x64 `
    -DORT_ROOT="$OrtRoot"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
if ($ConfigureOnly) { return }

Write-Host "build: Release x64"
& $cmake --build $BuildDir --config Release --parallel 8
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

if (-not $SkipPackage) {
    & (Join-Path $PSScriptRoot "pb-package-matting-cuda.ps1") `
        -BuildDir $BuildDir -OrtRoot $OrtRoot -CudnnDir $CudnnDir `
        -CudaBin $CudaBin -ModelDir $ModelDir -Dest $Dest
    if ($LASTEXITCODE -ne 0) { throw "packaging failed" }
}
