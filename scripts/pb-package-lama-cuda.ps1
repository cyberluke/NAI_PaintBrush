<#
.SYNOPSIS
  Package the native LaMa inpaint runtime into the VLC application tree.

.DESCRIPTION
  Verifies the pinned model SHA-256, then copies pb_lama_cuda.dll and its FULL
  dependency closure into <Dest>, plus the ONNX graph:

    <Dest>\pb_lama_cuda.dll
    <Dest>\onnxruntime*.dll
    <Dest>\cudnn*.dll
    <Dest>\cublas*.dll / cudart*.dll
    <Dest>\models\lama_fp32.onnx
    <Dest>\DEPENDENCIES.json      (versions + SHA-256 of every packaged file)
    <Dest>\NOTICE.txt

  The application's MinGW shim loads pb_lama_cuda.dll from this directory by
  absolute path and preloads the closure, so nothing depends on PATH or on a
  system-wide ONNX Runtime/CUDA install.

  The model is the Carve/LaMa-ONNX fp32 export (Apache-2.0) at the pinned
  revision. It is a FIXED 512 graph: the runtime crops a square context ROI,
  so this is not native-resolution synthesis.

.PARAMETER Dest
  Deployment directory. Defaults to <repo>\win64-meson\bin\lama.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$OrtRoot,
    [Parameter(Mandatory = $true)][string]$CudnnDir,
    [Parameter(Mandatory = $true)][string]$CudaBin,
    [Parameter(Mandatory = $true)][string]$ModelPath,
    [string]$ModelSha256 = "1faef5301d78db7dda502fe59966957ec4b79dd64e16f03ed96913c7a4eb68d6",
    [string]$Dest = ""
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Dest)) {
    $Dest = Join-Path $repo "win64-meson\bin\lama"
}

$dllCandidates = @(
    (Join-Path $BuildDir "bin\Release\pb_lama_cuda.dll"),
    (Join-Path $BuildDir "bin\pb_lama_cuda.dll"),
    (Join-Path $BuildDir "Release\pb_lama_cuda.dll")
)
$dll = $dllCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $dll) { throw "pb_lama_cuda.dll not found under $BuildDir" }

$ortNative = Join-Path $OrtRoot "runtimes\win-x64\native"
$cudaNames = @("cublas64_13.dll", "cublasLt64_13.dll", "cudart64_13.dll")
$ortNames = @("onnxruntime.dll", "onnxruntime_providers_cuda.dll",
              "onnxruntime_providers_shared.dll")
$cudnnNames = @("cudnn64_9.dll", "cudnn_graph64_9.dll", "cudnn_ops64_9.dll",
                "cudnn_cnn64_9.dll", "cudnn_adv64_9.dll", "cudnn_ext64_9.dll",
                "cudnn_heuristic64_9.dll", "cudnn_engines_precompiled64_9.dll",
                "cudnn_engines_runtime_compiled64_9.dll",
                "cudnn_engines_tensor_ir64_9.dll")

if (-not (Test-Path -LiteralPath $ModelPath)) {
    throw "LaMa model missing: $ModelPath"
}
$actualSha = (Get-FileHash -LiteralPath $ModelPath -Algorithm SHA256).Hash.ToLowerInvariant()
$expectedSha = $ModelSha256.ToLowerInvariant()
if ($actualSha -ne $expectedSha) {
    throw "LaMa model hash mismatch: expected $expectedSha, actual $actualSha"
}
$modelSize = (Get-Item -LiteralPath $ModelPath).Length

foreach ($name in $ortNames) {
    if (-not (Test-Path -LiteralPath (Join-Path $ortNative $name))) {
        throw "ONNX Runtime artifact missing: $name in $ortNative"
    }
}
foreach ($name in $cudnnNames) {
    if (-not (Test-Path -LiteralPath (Join-Path $CudnnDir $name))) {
        throw "cuDNN artifact missing: $name in $CudnnDir"
    }
}
foreach ($name in $cudaNames) {
    if (-not (Test-Path -LiteralPath (Join-Path $CudaBin $name))) {
        throw "CUDA runtime artifact missing: $name in $CudaBin"
    }
}

New-Item -ItemType Directory -Force -Path $Dest | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Dest "models") | Out-Null

Copy-Item -LiteralPath $dll -Destination (Join-Path $Dest "pb_lama_cuda.dll") -Force
foreach ($name in $ortNames) {
    Copy-Item -LiteralPath (Join-Path $ortNative $name) -Destination $Dest -Force
}
foreach ($name in $cudnnNames) {
    Copy-Item -LiteralPath (Join-Path $CudnnDir $name) -Destination $Dest -Force
}
foreach ($name in $cudaNames) {
    Copy-Item -LiteralPath (Join-Path $CudaBin $name) -Destination $Dest -Force
}
Copy-Item -LiteralPath $ModelPath -Destination (Join-Path $Dest "models\lama_fp32.onnx") -Force

$onnxDll = Join-Path $ortNative "onnxruntime.dll"
$verInfo = (Get-Item -LiteralPath $onnxDll).VersionInfo

$files = Get-ChildItem -LiteralPath $Dest -Recurse -File |
         Where-Object { $_.Name -notin @("DEPENDENCIES.json", "NOTICE.txt") }
$records = foreach ($file in $files) {
    [pscustomobject]@{
        path = $file.FullName.Substring($Dest.Length + 1).Replace('\', '/')
        bytes = $file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

$deps = [pscustomobject]@{
    schema = "nai-paintbrush.lama-deps/v1"
    createdUtc = (Get-Date).ToUniversalTime().ToString("o")
    abiVersion = 1
    runtime = [pscustomobject]@{
        onnxRuntime = "1.30.0"
        onnxRuntimeFileVersion = $verInfo.FileVersion
        onnxRuntimeProductVersion = $verInfo.ProductVersion
        cuda = "13.3"
        cudnn = "9.28.1.4"
        compiler = "MSVC 19.44 (Visual Studio 2022 17.14)"
        arch = "win-x64"
    }
    model = [pscustomobject]@{
        family = "big-lama"
        variant = "lama_fp32"
        sourceRepo = "Carve/LaMa-ONNX"
        sourceRevision = "c3c0c9e468934d62e79c329e35d82dd09ff8c444"
        file = "models/lama_fp32.onnx"
        bytes = $modelSize
        sha256 = $expectedSha
        license = "Apache-2.0"
        fixedInput = $true
        inputSize = 512
        inputs = @(
            "image: float32 [1,3,512,512], RGB 0..1",
            "mask:  float32 [1,1,512,512], 1 = masked"
        )
        output = "output: float32 [1,3,512,512] (this export clamps inpainted * 255)"
        limitation = "Fixed 512 context-ROI inference; not native-resolution synthesis. Unmasked pixels are preserved."
    }
    files = @($records)
}
$deps | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Dest "DEPENDENCIES.json") -Encoding utf8

@"
NAI Paintbrush — native LaMa inpainting runtime

This directory contains third-party components with their own licenses:

  ONNX Runtime 1.30.0 ............ MIT (Microsoft)
  NVIDIA cuDNN 9.28.1.4 .......... NVIDIA cuDNN Software License Agreement
  NVIDIA CUDA runtime libraries .. NVIDIA CUDA Toolkit End User License Agreement
  LaMa source .................... Apache-2.0 (advimman/lama)
  LaMa ONNX export + weights ..... Apache-2.0 (Carve/LaMa-ONNX)
                                   revision c3c0c9e468934d62e79c329e35d82dd09ff8c444

The graph (models/lama_fp32.onnx) is executed in-process by ONNX Runtime.
No PyTorch, Python, ComfyUI or OpenFill runtime is shipped or required. The
runtime verifies the model SHA-256 recorded in DEPENDENCIES.json before a
session is created.

The graph is a FIXED 512 export. NAI Paintbrush crops a square context ROI
around the selected mask (with dilation and padding), resizes it to 512,
decodes the result and feathers it back into the full canvas. Unmasked pixels
are preserved byte-for-byte. This is not native-resolution synthesis.

No GPL/LGPL code from VLC is linked into pb_lama_cuda.dll; it links only the
ONNX Runtime import library and the Windows SDK, and inspects the CUDA driver
dynamically.

--------------------------------------------------------------------------
MIT License (ONNX Runtime)

Copyright (c) Microsoft Corporation. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

--------------------------------------------------------------------------
Apache License 2.0 (LaMa source and Carve/LaMa-ONNX export)

The LaMa source and the ONNX export/weights are distributed under the Apache
License, Version 2.0. A copy of the license is available at:
  https://www.apache.org/licenses/LICENSE-2.0
Model card: https://huggingface.co/Carve/LaMa-ONNX

--------------------------------------------------------------------------
NVIDIA terms

CUDA Toolkit EULA: https://docs.nvidia.com/cuda/eula/index.html
cuDNN Software License Agreement:
  https://docs.nvidia.com/deeplearning/cudnn/sla/index.html
The NVIDIA DLLs are redistributed unmodified as runtime dependencies.
"@ | Set-Content -LiteralPath (Join-Path $Dest "NOTICE.txt") -Encoding utf8

$totalMb = [math]::Round((($files | Measure-Object -Property Length -Sum).Sum) / 1MB, 1)
Write-Output "packaged $($files.Count) files ($totalMb MB) -> $Dest"
Write-Output "onnxruntime product=$($verInfo.ProductVersion) file=$($verInfo.FileVersion)"
Write-Output "model sha256=$expectedSha bytes=$modelSize"
