<#
.SYNOPSIS
  Build and run the MinGW QImage -> C ABI human parser smoke.

.DESCRIPTION
  The MSVC smoke (human_parser_smoke.exe) proves the native runtime. This
  script proves the MinGW half the plugin actually uses:

    QImage -> paintbrush::human_parser::Runtime (shim) -> C ABI
           -> pb_human_parser_cuda.dll -> QImage masks

  It uses CPUExecutionProvider and asserts the exporter's exact grid counts.
  Only the small ORT core closure is staged; the CUDA provider is deliberately
  not copied (the MSVC smoke covers GPU).

  Why staging matters: this machine has an unrelated onnxruntime.dll 1.17.1 in
  C:\Windows\System32. Without preloading the packaged 1.30.0 core from the
  parser directory, the Windows loader would pick System32 and the parser ABI
  would fail. The shim's preload closure is what prevents that.

  The default fixture is the lossless PNG transcode of the FASHN model-card
  WebP. The MSVC smoke reads the original .webp through WIC; this Qt test uses
  PNG because the local Qt build has no WebP image plugin. Override -Fixture
  if the fixture lives elsewhere.

.EXAMPLE
  pwsh -File scripts/pb-test-human-parser-shim.ps1
#>
[CmdletBinding()]
param(
    [string]$ParserDll = "D:\_SATIN_AI\NAI-OS\gpu-human-parser\build-msvc\bin\Release\pb_human_parser_cuda.dll",
    [string]$OrtClosure = "D:\_SATIN_AI\NAI-OS\vlc\win64-meson\bin\matting",
    [string]$ModelDir = "D:\_SATIN_AI\models\fashn-human-parser",
    [string]$Fixture = "C:\Users\LUKES~1.COR\AppData\Local\Temp\kilo\fashn-example.png",
    [string]$StageDir = "D:\_SATIN_AI\NAI-OS\gpu-human-parser\shim-test",
    [string]$Compiler = "C:\msys64\mingw64\bin\g++.exe",
    [string]$QtRoot = "C:\msys64\mingw64"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

foreach ($path in @($ParserDll, $OrtClosure, $ModelDir, $Fixture, $Compiler, $QtRoot)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "missing input: $path"
    }
}
$model = Join-Path $ModelDir "fashn-human-parser.onnx"
$manifest = Join-Path $ModelDir "manifest.json"
if (-not (Test-Path -LiteralPath $model)) { throw "missing model: $model" }
if (-not (Test-Path -LiteralPath $manifest)) { throw "missing manifest: $manifest" }

$mingwBin = Split-Path -Parent $Compiler
$env:PATH = "$mingwBin;$env:PATH"

$qtVersion = Get-ChildItem -LiteralPath (Join-Path $QtRoot "include\qt6\QtCore") -Directory |
    Where-Object { $_.Name -match '^6\.' } |
    Sort-Object Name -Descending |
    Select-Object -First 1
if (-not $qtVersion) { throw "Qt6 version directory not found under $QtRoot" }
$v = $qtVersion.Name

$runtimeStage = Join-Path $StageDir "human-parser"
New-Item -ItemType Directory -Force -Path $runtimeStage | Out-Null
Copy-Item -LiteralPath $ParserDll -Destination (Join-Path $runtimeStage "pb_human_parser_cuda.dll") -Force
foreach ($name in @("onnxruntime.dll", "onnxruntime_providers_shared.dll")) {
    $source = Join-Path $OrtClosure $name
    if (-not (Test-Path -LiteralPath $source)) { throw "missing ORT core file: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $runtimeStage $name) -Force
}

$shim = Join-Path $repo "modules\gui\qt\paintbrush\gpu\pb_human_parser_shim.cpp"
$test = Join-Path $repo "modules\gui\qt\paintbrush\gpu\human_parser\tools\qimage_shim_smoke.cpp"
$exe = Join-Path $StageDir "qimage_shim_smoke.exe"

$includeArgs = @(
    "-I$(Join-Path $repo 'modules\gui\qt\paintbrush\gpu')",
    "-I$(Join-Path $QtRoot 'include\qt6')",
    "-I$(Join-Path $QtRoot 'include\qt6\QtCore')",
    "-I$(Join-Path $QtRoot "include\qt6\QtCore\$v")",
    "-I$(Join-Path $QtRoot "include\qt6\QtCore\$v\QtCore")",
    "-I$(Join-Path $QtRoot 'include\qt6\QtGui')",
    "-I$(Join-Path $QtRoot "include\qt6\QtGui\$v")",
    "-I$(Join-Path $QtRoot "include\qt6\QtGui\$v\QtGui")"
)
$compileArgs = @("-std=c++17", "-O2", "-Wall", "-Wextra") + $includeArgs + @(
    $shim, $test,
    "-L$(Join-Path $QtRoot 'lib')",
    "-lQt6Gui", "-lQt6Core",
    "-o", $exe
)

Write-Host "compile: $Compiler $($compileArgs -join ' ')"
& $Compiler @compileArgs
if ($LASTEXITCODE -ne 0) { throw "shim smoke compile failed" }

$env:PATH = "$StageDir;$OrtClosure;$mingwBin;$env:PATH"
$env:PB_HUMAN_PARSER_DIR = $runtimeStage
$env:QT_PLUGIN_PATH = Join-Path $QtRoot "plugins"
$env:QT_LOGGING_TO_CONSOLE = "1"
$env:QT_FORCE_STDERR_LOGGING = "1"

Write-Host "run: $exe"
& $exe $model $manifest $Fixture
if ($LASTEXITCODE -ne 0) { throw "shim smoke failed (exit $LASTEXITCODE)" }
