<#
.SYNOPSIS
  Build the CUDA SAM3 runtime and its narrow C ABI bridge with MSVC + CUDA.

.DESCRIPTION
  Reproducible build for the Windows GPU SAM3 integration:
    1. clone/check out the pinned greenjava/sam3.cpp `cuda` fork
    2. check out its pinned ggml `sam3-ops` submodule
    3. apply the NAI backend-query + CPU-tracking-thread patch
    4. build the SAM3 static runtime (MSVC host, CUDA 89)
    5. build sam3_cuda.dll (C ABI bridge) and the acceptance smoke tool
    6. deploy the runtime DLL closure into the VLC build's bin

  Source/build trees live OUTSIDE the VLC repository so the shared worktree
  stays clean. Default location: D:\_SATIN_AI\NAI-OS\gpu-sam3

.PARAMETER Clean
  Delete the source/build trees first (full rebuild).

.PARAMETER SkipDeploy
  Build only; do not copy DLLs into the VLC bin directory.

.EXAMPLE
  pwsh -File scripts/pb-build-sam3-cuda.ps1
  pwsh -File scripts/pb-build-sam3-cuda.ps1 -Clean
#>
[CmdletBinding()]
param(
    [string]$Base = "D:\_SATIN_AI\NAI-OS\gpu-sam3",
    [string]$CudaArch = "89",
    [string]$CudaVersion = "v13.3",
    [string]$Model = "D:\_SATIN_AI\models\sam3-f16.ggml",
    [switch]$Clean,
    [switch]$SkipDeploy
)

$ErrorActionPreference = "Stop"

# Pinned pair used for the verified build. The PR referenced ggml 499c8a76;
# the fork's tested submodule pin is 9fff8202, which contains the CUDA
# WIN_PART/WIN_UNPART, attention and transposed-convolution work.
$Sam3Url    = "https://github.com/greenjava/sam3.cpp.git"
$Sam3Commit = "c0600a7ffce9ae097b112cf02ed663a73f94c169"
$GgmlCommit = "9fff8202695fb970e138f0692cd340ffa0f65ead"

$repo = Split-Path -Parent $PSScriptRoot
$sam3Dir = Join-Path $Base "sam3.cpp"
$sam3Build = Join-Path $Base "build-msvc"
$bridgeDir = Join-Path $repo "modules\gui\qt\paintbrush\gpu"
$bridgeBuild = Join-Path $Base "bridge-msvc"
$patch = Join-Path $bridgeDir "patches\sam3-backend-query-and-cpu-threads.patch"

if ($Clean) {
    Write-Host "Removing $Base"
    Remove-Item -Recurse -Force $Base -ErrorAction SilentlyContinue
}

New-Item -ItemType Directory -Force -Path $Base | Out-Null

if (-not (Test-Path (Join-Path $sam3Dir ".git"))) {
    Write-Host "Cloning $Sam3Url"
    git clone --recursive $Sam3Url $sam3Dir
} else {
    git -C $sam3Dir fetch --all --tags
}
git -C $sam3Dir checkout $Sam3Commit
git -C $sam3Dir submodule update --init --recursive
git -C (Join-Path $sam3Dir "ggml") checkout $GgmlCommit

if (Test-Path $patch) {
    $check = git -C $sam3Dir apply --check $patch 2>&1
    if ($LASTEXITCODE -eq 0) {
        git -C $sam3Dir apply $patch
        Write-Host "Applied $(Split-Path -Leaf $patch)"
    } else {
        Write-Host "Patch already applied or not applicable: $check"
    }
}

Write-Host "Configuring SAM3 CUDA (MSVC host, sm_$CudaArch)"
cmake -S $sam3Dir -B $sam3Build -G "Visual Studio 17 2022" -A x64 `
    -DSAM3_CUDA=ON -DSAM3_METAL=OFF -DSAM3_BUILD_EXAMPLES=OFF `
    -DSAM3_BUILD_TESTS=OFF -DCMAKE_CUDA_ARCHITECTURES=$CudaArch `
    -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "SAM3 configure failed" }

Write-Host "Building sam3"
cmake --build $sam3Build --config Release --target sam3 --parallel 12
if ($LASTEXITCODE -ne 0) { throw "SAM3 build failed" }

Write-Host "Building sam3_cuda.dll bridge + smoke tool"
cmake -S $bridgeDir -B $bridgeBuild -G "Visual Studio 17 2022" -A x64 `
    -DSAM3_SOURCE_DIR="$sam3Dir" -DSAM3_BUILD_DIR="$sam3Build" `
    -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "Bridge configure failed" }
cmake --build $bridgeBuild --config Release --parallel 12
if ($LASTEXITCODE -ne 0) { throw "Bridge build failed" }

$bridgeBin = Join-Path $bridgeBuild "bin"
Write-Host "Built:"
Get-ChildItem $bridgeBin -Filter "*.dll" |
    Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize

if (-not $SkipDeploy) {
    & (Join-Path $PSScriptRoot "pb-package-sam3-cuda.ps1") `
        -BridgeBin $bridgeBin `
        -CudaBin "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\$CudaVersion\bin\x64" `
        -Dest (Join-Path $repo "win64-meson\bin") `
        -Model $Model
}

Write-Host "SAM3 CUDA build complete."
Write-Host "Pins: sam3 $Sam3Commit / ggml $GgmlCommit"
