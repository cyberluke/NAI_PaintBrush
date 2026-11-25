# NAI Paintbrush — standalone creative recipe harness.
# Compiles tests/creative_recipe_test.cpp with pb_creative_art.cpp using the
# local Qt 6 Core/Gui. pb_creative_recipe.hpp has no VLC, QObject, controller
# or document dependency, so this needs no VLC build, no GUI and no plugin.
# It exercises the EXACT production builder used by the async commit.
#
#   pwsh scripts/pb-creative-recipe-tests.ps1
#   pwsh scripts/pb-creative-recipe-tests.ps1 -QtRoot C:\msys64\mingw64
#   pwsh scripts/pb-creative-recipe-tests.ps1 -FourK
#     -FourK adds the opt-in 3840x2160 native build/compose for Neon Idol and
#     the holo-liquid material. It is heavier (~0.5 GB peak) and not part of the
#     default fast harness.

[CmdletBinding()]
param(
    [string]$QtRoot = "C:\msys64\mingw64",
    [switch]$FourK
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$source = Join-Path $repoRoot "modules\gui\qt\paintbrush\tests\creative_recipe_test.cpp"
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
$exe = Join-Path $outDir "pb-creative-recipe-test.exe"

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

Write-Output "--- compiling creative_recipe_test ---"
& $gxx @args 2>&1 | ForEach-Object { Write-Output $_ }
if ($LASTEXITCODE -ne 0) {
    Write-Output "CreativeRecipeTests|FAIL (compile)"
    exit 1
}

Write-Output "--- running creative_recipe_test ---"
$runArgs = @()
if ($FourK) { $runArgs += "--4k" }
& $exe @runArgs 2>&1 | ForEach-Object { Write-Output $_ }
exit $LASTEXITCODE
