<#
.SYNOPSIS
  Install (or update) the FASHN human-parser runtime and model for NAI Paintbrush.

.DESCRIPTION
  This is both a post-installer and an on-demand downloader. The app checks the
  user install directory at launch; the Play Pad's "Get the fashion parser"
  action runs this script. The script can also run from an MSIX post-install
  step or manually.

  Sources, in order:
    -ManifestPath <json>  local manifest
    -ManifestUrl  <url>   remote manifest (downloaded)
    -SourceDir    <dir>   already-unpacked assets (offline/dev)
    -BundlePath   <zip>   already-downloaded bundle

  The bundle contains pb_human_parser_cuda.dll, models/fashn-human-parser.onnx
  and models/manifest.json. The ONNX Runtime/CUDA closure is copied from the
  app's matting runtime when present; a bundle may also carry its own closure.

  Output is line-oriented so the app can show progress:
    PBHP-PROGRESS <0-100> <text>
    PBHP-INSTALLED <path>
    PBHP-ERROR <text>

  The install is staged in a temp directory, each required file is hash-checked
  when the manifest provides a hash, and only then copied into place.

.PARAMETER InstallDir
  User-writable target. Defaults to
  %LOCALAPPDATA%\NAI Paintbrush\human-parser.

.PARAMETER AppDir
  The app directory; used to locate the shared matting ORT/CUDA closure.
#>
[CmdletBinding(DefaultParameterSetName = "Url")]
param(
    [string]$ManifestUrl,
    [string]$ManifestSha256,
    [string]$ManifestPath,
    [string]$SourceDir,
    [string]$BundlePath,
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA "NAI Paintbrush\human-parser"),
    [string]$AppDir = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

function Write-ProgressLine([int]$percent, [string]$text)
{
    Write-Output ("PBHP-PROGRESS {0} {1}" -f [Math]::Max(0, [Math]::Min(100, $percent)), $text)
}

function Get-Sha256([string]$path)
{
    (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
}

function Copy-Closure([string]$from, [string]$to)
{
    if (-not (Test-Path -LiteralPath $from)) { return $false }
    $names = Get-ClosureNames
    foreach ($name in $names)
    {
        if (-not (Test-Path -LiteralPath (Join-Path $from $name))) { return $false }
    }
    foreach ($name in $names)
    {
        Copy-Item -LiteralPath (Join-Path $from $name) -Destination (Join-Path $to $name) -Force
    }
    return $true
}

function Get-ClosureNames()
{
    @(
        "onnxruntime.dll",
        "onnxruntime_providers_shared.dll",
        "onnxruntime_providers_cuda.dll",
        "cublas64_13.dll", "cublasLt64_13.dll", "cudart64_13.dll",
        "cudnn64_9.dll", "cudnn_graph64_9.dll", "cudnn_engines_precompiled64_9.dll",
        "cudnn_engines_runtime_compiled64_9.dll", "cudnn_heuristic64_9.dll",
        "cudnn_ops64_9.dll", "cudnn_adv64_9.dll", "cudnn_cnn64_9.dll",
        "cudnn_ext64_9.dll", "cudnn_engines_tensor_ir64_9.dll"
    )
}

$temp = Join-Path $env:TEMP ("pb-human-parser-install-" + [guid]::NewGuid().ToString("N"))
$manifest = $null
$installManifestPath = $null
$source = $null
$bundleZip = $null

New-Item -ItemType Directory -Force -Path $temp | Out-Null
try
{
    Write-ProgressLine 2 "Checking the fashion parser assets"

    if ($ManifestPath)
    {
        if (-not (Test-Path -LiteralPath $ManifestPath)) { throw "manifest not found: $ManifestPath" }
        $installManifestPath = (Resolve-Path -LiteralPath $ManifestPath).Path
        $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    }
    elseif ($ManifestUrl)
    {
        if (-not $ManifestSha256) {
            throw "a remote manifest requires -ManifestSha256 so the download has a pinned trust anchor"
        }
        Write-ProgressLine 4 "Fetching the parser manifest"
        $manifestFile = Join-Path $temp "manifest.json"
        Invoke-WebRequest -Uri $ManifestUrl -OutFile $manifestFile -UseBasicParsing
        $actualManifestSha = Get-Sha256 $manifestFile
        if ($actualManifestSha -ne $ManifestSha256.ToLowerInvariant()) {
            throw "manifest SHA-256 mismatch (expected $ManifestSha256, got $actualManifestSha)"
        }
        $installManifestPath = $manifestFile
        $manifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
    }

    if ($BundlePath)
    {
        if (-not (Test-Path -LiteralPath $BundlePath)) { throw "bundle not found: $BundlePath" }
        $bundleZip = $BundlePath
    }
    elseif ($manifest -and $manifest.bundle -and $manifest.bundle.url)
    {
        $bundleZip = Join-Path $temp "bundle.zip"
        Write-ProgressLine 6 "Downloading the parser bundle"
        Invoke-WebRequest -Uri $manifest.bundle.url -OutFile $bundleZip -UseBasicParsing
    }

    if ($bundleZip)
    {
        if ($ManifestUrl -and -not ($manifest -and $manifest.bundle -and $manifest.bundle.sha256)) {
            throw "a pinned remote install requires bundle.sha256 in the manifest"
        }
        if ($manifest -and $manifest.bundle -and $manifest.bundle.sha256)
        {
            Write-ProgressLine 55 "Verifying the parser bundle"
            $actual = Get-Sha256 $bundleZip
            if ($actual -ne $manifest.bundle.sha256.ToLowerInvariant())
            {
                throw "bundle SHA-256 mismatch (expected $($manifest.bundle.sha256), got $actual)"
            }
        }
        Write-ProgressLine 60 "Unpacking the parser bundle"
        $unpacked = Join-Path $temp "unpacked"
        Expand-Archive -LiteralPath $bundleZip -DestinationPath $unpacked -Force
        $source = $unpacked
    }
    elseif ($SourceDir)
    {
        if (-not (Test-Path -LiteralPath $SourceDir)) { throw "source not found: $SourceDir" }
        $source = $SourceDir
    }

    if (-not $source) { throw "no install source: provide -ManifestUrl, -ManifestPath, -BundlePath or -SourceDir" }

    $runtime = Get-ChildItem -LiteralPath $source -Filter "pb_human_parser_cuda.dll" -File -Recurse |
        Select-Object -First 1
    $model = Get-ChildItem -LiteralPath $source -Filter "fashn-human-parser.onnx" -File -Recurse |
        Select-Object -First 1
    $modelManifest = Get-ChildItem -LiteralPath $source -Filter "manifest.json" -File -Recurse |
        Where-Object { -not $installManifestPath -or $_.FullName -ne $installManifestPath } |
        Select-Object -First 1
    if (-not $runtime -or -not $model -or -not $modelManifest)
    {
        throw "the source is incomplete: runtime=$([bool]$runtime) model=$([bool]$model) manifest=$([bool]$modelManifest)"
    }

    Write-ProgressLine 65 "Verifying the parser runtime and model"
    $entries = @(
        @{ path = "pb_human_parser_cuda.dll"; file = $runtime.FullName },
        @{ path = "models/fashn-human-parser.onnx"; file = $model.FullName },
        @{ path = "models/manifest.json"; file = $modelManifest.FullName }
    )
    if ($ManifestUrl)
    {
        if (-not $manifest -or -not $manifest.files) {
            throw "a pinned remote install requires manifest.files with SHA-256 entries"
        }
        foreach ($entry in $entries) {
            $prop = $manifest.files.PSObject.Properties[$entry.path]
            if (-not ($prop -and $prop.Value -and $prop.Value.sha256)) {
                throw "a pinned remote install requires a SHA-256 for $($entry.path)"
            }
        }
    }
    if ($manifest -and $manifest.files)
    {
        foreach ($entry in $entries)
        {
            $prop = $manifest.files.PSObject.Properties[$entry.path]
            if ($prop -and $prop.Value -and $prop.Value.sha256)
            {
                $expected = [string]$prop.Value.sha256
                $actual = Get-Sha256 $entry.file
                if ($actual -ne $expected.ToLowerInvariant())
                {
                    throw "$($entry.path) SHA-256 mismatch (expected $expected, got $actual)"
                }
            }
        }
    }

    $stage = Join-Path $temp "stage"
    New-Item -ItemType Directory -Force -Path (Join-Path $stage "models") | Out-Null
    Copy-Item -LiteralPath $runtime.FullName -Destination (Join-Path $stage "pb_human_parser_cuda.dll") -Force
    Copy-Item -LiteralPath $model.FullName -Destination (Join-Path $stage "models\fashn-human-parser.onnx") -Force
    Copy-Item -LiteralPath $modelManifest.FullName -Destination (Join-Path $stage "models\manifest.json") -Force

    Write-ProgressLine 75 "Assembling the ONNX Runtime closure"
    $closureCopied = $false
    if ($AppDir)
    {
        foreach ($candidate in @(
            (Join-Path $AppDir "matting"),
            (Join-Path (Split-Path $AppDir -Parent) "matting"),
            (Join-Path $AppDir "human-parser")
        ))
        {
            if (Copy-Closure $candidate $stage) { $closureCopied = $true; break }
        }
    }
    foreach ($candidate in @($source, (Join-Path $source "matting")))
    {
        if (Copy-Closure $candidate $stage) { $closureCopied = $true; break }
    }
    if (-not $closureCopied)
    {
        throw "the ONNX Runtime/CUDA closure was not found; install the matting runtime or provide it in the bundle"
    }

    $metadata = [ordered]@{
        schema = 1
        installer = "pb-install-human-parser.ps1"
        installedAt = (Get-Date).ToUniversalTime().ToString("o")
        source = $source
        manifestUrl = $ManifestUrl
        runtimeSha256 = (Get-Sha256 (Join-Path $stage "pb_human_parser_cuda.dll"))
        modelSha256 = (Get-Sha256 (Join-Path $stage "models\fashn-human-parser.onnx"))
        manifestSha256 = $ManifestSha256
        closure = @(Get-ClosureNames)
    }
    $metadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $stage "installed.json") -Encoding UTF8

    Write-ProgressLine 85 "Installing into the user data directory"
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $InstallDir "models") | Out-Null
    Get-ChildItem -LiteralPath $stage -File | Copy-Item -Destination $InstallDir -Force
    Get-ChildItem -LiteralPath (Join-Path $stage "models") -File |
        Copy-Item -Destination (Join-Path $InstallDir "models") -Force

    Write-ProgressLine 100 "Fashion parser installed"
    Write-Output "PBHP-INSTALLED $InstallDir"
}
catch
{
    Write-Output "PBHP-ERROR $($_.Exception.Message)"
    exit 1
}
finally
{
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
