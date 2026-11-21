# NAI Paintbrush — standalone C++ media-logic harness.
# Compiles tests/media_logic_test.cpp with the local Qt 6 Core/Gui and runs it.
# pb_media_logic.hpp deliberately has no VLC dependency, so this needs no VLC
# build, no GUI, and no plugin.

[CmdletBinding()]
param(
    [string]$QtRoot = "C:\msys64\mingw64",
    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..\..")).Path
$source = Join-Path $repoRoot "modules\gui\qt\paintbrush\tests\media_logic_test.cpp"
if (-not (Test-Path -LiteralPath $source)) {
    Write-Error "media_logic_test.cpp not found at $source"
    exit 2
}

$gxx = Join-Path $QtRoot "bin\g++.exe"
if (-not (Test-Path -LiteralPath $gxx)) {
    Write-Error "g++.exe not found at $gxx"
    exit 2
}

$includeDirs = @(
    (Join-Path $QtRoot "include\qt6"),
    (Join-Path $QtRoot "include\qt6\QtCore"),
    (Join-Path $QtRoot "include\qt6\QtGui"),
    (Join-Path $repoRoot "modules\gui\qt\paintbrush")
)

$outDir = Join-Path $env:TEMP "kilo"
if (-not (Test-Path -LiteralPath $outDir)) {
    New-Item -ItemType Directory -Path $outDir | Out-Null
}
$exe = Join-Path $outDir "pb-media-logic-test.exe"

$args = @("-std=c++17", "-O2")
foreach ($dir in $includeDirs) { $args += "-I$dir" }
$args += $source
$args += "-o"
$args += $exe
$args += "-L" + (Join-Path $QtRoot "lib")
$args += "-lQt6Gui"
$args += "-lQt6Core"

$env:PATH = "$(Join-Path $QtRoot 'bin');$env:PATH"

Write-Output "--- compiling media_logic_test ---"
& $gxx @args 2>&1 | ForEach-Object { Write-Output $_ }
if ($LASTEXITCODE -ne 0) {
    Write-Output "MediaLogicTests|FAIL (compile)"
    exit 1
}

Write-Output "--- running media_logic_test ---"
& $exe 2>&1 | ForEach-Object { Write-Output $_ }
exit $LASTEXITCODE
