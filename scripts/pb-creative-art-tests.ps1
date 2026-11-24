# NAI Paintbrush — standalone creative-art harness.
# Compiles tests/creative_art_test.cpp with pb_creative_art.cpp using the local
# Qt 6 Core/Gui. pb_creative_art.hpp has no VLC dependency, so this needs no VLC
# build, no GUI, and no plugin.
#
#   pwsh scripts/pb-creative-art-tests.ps1
#   pwsh scripts/pb-creative-art-tests.ps1 -QtRoot C:\msys64\mingw64

[CmdletBinding()]
param(
    [string]$QtRoot = "C:\msys64\mingw64"
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$source = Join-Path $repoRoot "modules\gui\qt\paintbrush\tests\creative_art_test.cpp"
$art = Join-Path $repoRoot "modules\gui\qt\paintbrush\pb_creative_art.cpp"
foreach ($file in @($source, $art)) {
    if (-not (Test-Path -LiteralPath $file)) {
        Write-Error "missing source: $file"
        exit 2
    }
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
$exe = Join-Path $outDir "pb-creative-art-test.exe"

$args = @("-std=c++17", "-O2", "-DPB_CREATIVE_ART_STANDALONE")
foreach ($dir in $includeDirs) { $args += "-I$dir" }
$args += $source
$args += $art
$args += "-o"
$args += $exe
$args += "-L" + (Join-Path $QtRoot "lib")
$args += "-lQt6Gui"
$args += "-lQt6Core"

$env:PATH = "$(Join-Path $QtRoot 'bin');$env:PATH"
$env:QT_PLUGIN_PATH = Join-Path $QtRoot "share\qt6\plugins"

Write-Output "--- compiling creative_art_test ---"
& $gxx @args 2>&1 | ForEach-Object { Write-Output $_ }
if ($LASTEXITCODE -ne 0) {
    Write-Output "CreativeArtTests|FAIL (compile)"
    exit 1
}

Write-Output "--- running creative_art_test ---"
& $exe 2>&1 | ForEach-Object { Write-Output $_ }
exit $LASTEXITCODE
