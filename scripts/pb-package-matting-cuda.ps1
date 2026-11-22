<#
.SYNOPSIS
  Package the native neural matting runtime into the VLC application tree.

.DESCRIPTION
  Copies pb_matting_cuda.dll and its FULL dependency closure into
  <Dest>, plus the ONNX graphs and their manifest:

    <Dest>\pb_matting_cuda.dll
    <Dest>\onnxruntime*.dll
    <Dest>\cudnn*.dll
    <Dest>\cublas*.dll / cudart*.dll
    <Dest>\models\vitmatte-s-512.onnx
    <Dest>\models\vitmatte-s-1024.onnx
    <Dest>\models\manifest.json
    <Dest>\DEPENDENCIES.json      (versions + SHA-256 of every packaged file)
    <Dest>\NOTICE.txt

  The application's MinGW shim loads pb_matting_cuda.dll from this directory
  by absolute path and preloads the closure, so nothing depends on PATH or on
  a system-wide ONNX Runtime/CUDA install.

.PARAMETER Dest
  Deployment directory. Defaults to <repo>\win64-meson\bin\matting.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$OrtRoot,
    [Parameter(Mandatory = $true)][string]$CudnnDir,
    [Parameter(Mandatory = $true)][string]$CudaBin,
    [Parameter(Mandatory = $true)][string]$ModelDir,
    [string]$Dest = ""
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Dest)) {
    $Dest = Join-Path $repo "win64-meson\bin\matting"
}

$dllCandidates = @(
    (Join-Path $BuildDir "bin\Release\pb_matting_cuda.dll"),
    (Join-Path $BuildDir "bin\pb_matting_cuda.dll"),
    (Join-Path $BuildDir "Release\pb_matting_cuda.dll")
)
$dll = $dllCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $dll) { throw "pb_matting_cuda.dll not found under $BuildDir" }

$ortNative = Join-Path $OrtRoot "runtimes\win-x64\native"
$cudaNames = @("cublas64_13.dll", "cublasLt64_13.dll", "cudart64_13.dll")
$ortNames = @("onnxruntime.dll", "onnxruntime_providers_cuda.dll",
              "onnxruntime_providers_shared.dll")
$cudnnNames = @("cudnn64_9.dll", "cudnn_graph64_9.dll", "cudnn_ops64_9.dll",
                "cudnn_cnn64_9.dll", "cudnn_adv64_9.dll", "cudnn_ext64_9.dll",
                "cudnn_heuristic64_9.dll", "cudnn_engines_precompiled64_9.dll",
                "cudnn_engines_runtime_compiled64_9.dll",
                "cudnn_engines_tensor_ir64_9.dll")

$models = @("vitmatte-s-512.onnx", "vitmatte-s-1024.onnx", "manifest.json")
foreach ($name in $models) {
    if (-not (Test-Path -LiteralPath (Join-Path $ModelDir $name))) {
        throw "model artifact missing: $name in $ModelDir"
    }
}
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

Copy-Item -LiteralPath $dll -Destination (Join-Path $Dest "pb_matting_cuda.dll") -Force
foreach ($name in $ortNames) {
    Copy-Item -LiteralPath (Join-Path $ortNative $name) -Destination $Dest -Force
}
foreach ($name in $cudnnNames) {
    Copy-Item -LiteralPath (Join-Path $CudnnDir $name) -Destination $Dest -Force
}
foreach ($name in $cudaNames) {
    Copy-Item -LiteralPath (Join-Path $CudaBin $name) -Destination $Dest -Force
}
foreach ($name in $models) {
    Copy-Item -LiteralPath (Join-Path $ModelDir $name) -Destination (Join-Path $Dest "models") -Force
}

$manifest = Get-Content -LiteralPath (Join-Path $ModelDir "manifest.json") -Raw | ConvertFrom-Json
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
    schema = "nai-paintbrush.matting-deps/v1"
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
        family = $manifest.model.family
        variant = $manifest.model.variant
        weightsRepo = $manifest.model.weightsRepo
        weightsRevision = $manifest.model.weightsRevision
        weightsSha256 = $manifest.model.weightsSha256
        weightsLicense = $manifest.model.weightsLicense
        sourceLicense = $manifest.model.sourceLicense
        precision = $manifest.model.precision
        opset = $manifest.runtime.opset
        graphs = @($manifest.graphs | ForEach-Object {
            [pscustomobject]@{ size = $_.size; file = $_.file; sha256 = $_.sha256 }
        })
    }
    files = @($records)
}
$deps | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Dest "DEPENDENCIES.json") -Encoding utf8

@"
NAI Paintbrush — neural matting runtime

This directory contains third-party components with their own licenses:

  ONNX Runtime 1.30.0 ............ MIT (Microsoft)
  NVIDIA cuDNN 9.28.1.4 .......... NVIDIA cuDNN Software License Agreement
  NVIDIA CUDA runtime libraries .. NVIDIA CUDA Toolkit End User License Agreement
  ViTMatte source ................ MIT (hustvl)
  ViTMatte small checkpoint ...... Apache-2.0 (model card), exported to ONNX

The ViTMatte checkpoint was converted with scripts/pb-export-matting-model.py
from the pinned Hugging Face revision recorded in
DEPENDENCIES.json and models/manifest.json. The ONNX graphs are byte-verified
against those hashes by the runtime before a session is created.

No GPL/LGPL code from VLC is linked into pb_matting_cuda.dll; it links only the
ONNX Runtime import library and the Windows SDK, and loads the CUDA driver
dynamically.
"@ | Set-Content -LiteralPath (Join-Path $Dest "NOTICE.txt") -Encoding utf8

$totalMb = [math]::Round((($files | Measure-Object -Property Length -Sum).Sum) / 1MB, 1)
Write-Output "packaged $($files.Count) files ($totalMb MB) -> $Dest"
Write-Output "onnxruntime product=$($verInfo.ProductVersion) file=$($verInfo.FileVersion)"
Write-Output "graphs: $(($manifest.graphs | ForEach-Object { "$($_.file) [$($_.size)]" }) -join ', ')"
