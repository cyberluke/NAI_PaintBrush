<#
.SYNOPSIS
  Build pb_human_parser_cuda.dll and human_parser_smoke.exe with MSVC.

.DESCRIPTION
  The FASHN human parser is a leaf MSVC/ONNX Runtime component, exactly like
  the neural matting runtime. It is built in a dedicated directory and must not
  be mixed into the MinGW win64-meson tree.

  Defaults reuse the verified local ORT/CUDA dependency cache. There is no
  package step: the smoke runs against the deployed ORT closure via
  --runtime-dir, and the application loader resolves <exe>\human-parser.

.EXAMPLE
  pwsh -File scripts/pb-build-human-parser.ps1
  pwsh -File scripts/pb-build-human-parser.ps1 -ConfigureOnly
#>
[CmdletBinding()]
param(
    [string]$OrtRoot = "D:\_SATIN_AI\NAI-OS\gpu-matting\deps\ort-1.30.0",
    [string]$BuildDir = "D:\_SATIN_AI\NAI-OS\gpu-human-parser\build-msvc",
    [string]$Source = "",
    [switch]$ConfigureOnly
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Source)) {
    $Source = Join-Path $repo "modules\gui\qt\paintbrush\gpu\human_parser"
}

if (-not (Test-Path -LiteralPath $OrtRoot)) { throw "ORT_ROOT missing: $OrtRoot" }
if (-not (Test-Path -LiteralPath (Join-Path $Source "CMakeLists.txt"))) {
    throw "human parser source missing: $Source"
}
$cmake = (Get-Command cmake -ErrorAction Stop).Source

Write-Host "configure: $Source"
& $cmake -S $Source -B $BuildDir -G "Visual Studio 17 2022" -A x64 `
    -DORT_ROOT="$OrtRoot"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
if ($ConfigureOnly) { return }

Write-Host "build: Release x64 -> $BuildDir"
& $cmake --build $BuildDir --config Release --parallel 8
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

Write-Host "artifacts:"
Get-ChildItem -LiteralPath (Join-Path $BuildDir "bin") -ErrorAction SilentlyContinue |
    Select-Object Name, Length
