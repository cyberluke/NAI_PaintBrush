<#
.SYNOPSIS
  Deploy the CUDA SAM3 runtime closure into the VLC application bin.

.DESCRIPTION
  Copies the MSVC/CUDA bridge, its ggml dependency DLLs and the CUDA runtime
  DLLs into the directory next to vlc.exe/paintbrush.exe. The MinGW plugin
  loads sam3_cuda.dll through the C ABI; the bridge resolves its dependencies
  from its own directory (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR).

  Prints sizes and SHA-256 hashes of the deployed files and (optionally) the
  model, so a build can be reproduced and audited.

.PARAMETER BundleMsvc
  Also copy msvcp140.dll / vcruntime140*.dll from System32 for a standalone
  distribution. On a machine with the VC++ redistributable this is optional.

.EXAMPLE
  pwsh -File scripts/pb-package-sam3-cuda.ps1 -Model D:\_SATIN_AI\models\sam3-f16.ggml
#>
[CmdletBinding()]
param(
    [string]$BridgeBin = "D:\_SATIN_AI\NAI-OS\gpu-sam3\bridge-msvc\bin",
    [string]$CudaBin = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3\bin\x64",
    [string]$Dest = "D:\_SATIN_AI\NAI-OS\vlc\win64-meson\bin",
    [string]$Model = "",
    [switch]$BundleMsvc
)

$ErrorActionPreference = "Stop"

$required = @(
    "sam3_cuda.dll",
    "ggml.dll",
    "ggml-base.dll",
    "ggml-cpu.dll",
    "ggml-cuda.dll",
    "cudart64_13.dll",
    "cublas64_13.dll",
    "cublasLt64_13.dll"
)

New-Item -ItemType Directory -Force -Path $Dest | Out-Null

foreach ($name in $required) {
    $source = if ($name -like "cudart*" -or $name -like "cublas*") {
        Join-Path $CudaBin $name
    } else {
        Join-Path $BridgeBin $name
    }
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Missing runtime dependency: $source"
    }
    $target = Join-Path $Dest $name
    Copy-Item -LiteralPath $source -Destination $target -Force
}

if ($BundleMsvc) {
    foreach ($name in @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")) {
        $source = Join-Path $env:SystemRoot "System32\$name"
        if (Test-Path -LiteralPath $source) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $Dest $name) -Force
        }
    }
}

"Deployed runtime closure to $Dest"
foreach ($name in $required) {
    $target = Join-Path $Dest $name
    $item = Get-Item -LiteralPath $target
    $hash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    "{0} {1,12} {2}" -f $name, $item.Length, $hash
}

if ($Model -ne "") {
    if (-not (Test-Path -LiteralPath $Model)) {
        throw "Model not found: $Model"
    }
    $item = Get-Item -LiteralPath $Model
    $hash = (Get-FileHash -LiteralPath $Model -Algorithm SHA256).Hash
    "MODEL {0} {1} {2}" -f $Model, $item.Length, $hash
}
