<#
.SYNOPSIS
  Build pb_lama_cuda.dll (MSVC + ONNX Runtime CUDA) and package it.

.DESCRIPTION
  One command for the native LaMa inpaint component:

    1. configure CMake with the pinned ONNX Runtime package;
    2. build Release x64 with the installed MSVC toolchain;
    3. verify the pinned lama_fp32.onnx SHA-256;
    4. package the DLL closure + the model into <repo>\win64-meson\bin\lama.

  Defaults point at the verified local dependency cache under
  D:\_SATIN_AI\NAI-OS\gpu-matting and the downloaded graph under
  D:\_SATIN_AI\models\lama. Override the paths for another machine; the
  versions and model hash are pinned in the packaging manifest
  (DEPENDENCIES.json).

.EXAMPLE
  pwsh -File scripts/pb-build-lama-cuda.ps1
  pwsh -File scripts/pb-build-lama-cuda.ps1 -ConfigureOnly
#>
[CmdletBinding()]
param(
    [string]$OrtRoot = "D:\_SATIN_AI\NAI-OS\gpu-matting\deps\ort-1.30.0",
    [string]$CudnnDir = "D:\_SATIN_AI\NAI-OS\gpu-matting\deps\cudnn-9.28.1.4",
    [string]$CudaBin = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3\bin\x64",
    [string]$ModelPath = "D:\_SATIN_AI\models\lama\lama_fp32.onnx",
    [string]$ModelSha256 = "1faef5301d78db7dda502fe59966957ec4b79dd64e16f03ed96913c7a4eb68d6",
    [string]$BuildDir = "D:\_SATIN_AI\NAI-OS\gpu-matting\build-lama-msvc",
    [string]$Dest = "",
    [switch]$ConfigureOnly,
    [switch]$SkipPackage
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$source = Join-Path $repo "modules\gui\qt\paintbrush\gpu\lama"

if (-not (Test-Path -LiteralPath $OrtRoot)) { throw "ORT_ROOT missing: $OrtRoot" }
if (-not (Test-Path -LiteralPath $ModelPath)) { throw "LaMa model missing: $ModelPath" }
if (-not (Test-Path -LiteralPath (Join-Path $source "CMakeLists.txt"))) {
    throw "LaMa source missing: $source"
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
    & (Join-Path $PSScriptRoot "pb-package-lama-cuda.ps1") `
        -BuildDir $BuildDir -OrtRoot $OrtRoot -CudnnDir $CudnnDir `
        -CudaBin $CudaBin -ModelPath $ModelPath -ModelSha256 $ModelSha256 `
        -Dest $Dest
    if ($LASTEXITCODE -ne 0) { throw "packaging failed" }
}
