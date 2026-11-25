# NAI Paintbrush — remote human-parser installer acceptance.
#
# Serves a small synthetic bundle over a local HTTP server and exercises the
# exact remote path the app uses:
#   1. a wrong pinned manifest SHA-256 must be rejected before anything is
#      installed;
#   2. the correct pin must download, verify the bundle and per-file hashes,
#      find the model manifest inside the unpacked temp tree, copy the complete
#      ORT/CUDA closure, and write installed.json.
#
# The synthetic files are tiny; this validates the installer plumbing, not
# parser inference. Run from the repository root:
#
#   pwsh scripts/pb-test-human-parser-remote-install.ps1
#   pwsh scripts/pb-test-human-parser-remote-install.ps1 -Port 8766
#   pwsh scripts/pb-test-human-parser-remote-install.ps1 -Keep

[CmdletBinding()]
param(
    [int]$Port = 8765,
    [switch]$Keep
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$installer = Join-Path $repoRoot "scripts\pb-install-human-parser.ps1"
if (-not (Test-Path -LiteralPath $installer)) {
    throw "installer not found: $installer"
}

$pwsh = (Get-Command pwsh -ErrorAction SilentlyContinue).Source
if (-not $pwsh) {
    throw "pwsh is required to run this acceptance script"
}
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) {
    $python = "C:\msys64\mingw64\bin\python.exe"
}
if (-not (Test-Path -LiteralPath $python)) {
    throw "python not found; cannot start the local HTTP server"
}

$closure = @(
    "onnxruntime.dll",
    "onnxruntime_providers_shared.dll",
    "onnxruntime_providers_cuda.dll",
    "cublas64_13.dll", "cublasLt64_13.dll", "cudart64_13.dll",
    "cudnn64_9.dll", "cudnn_graph64_9.dll", "cudnn_engines_precompiled64_9.dll",
    "cudnn_engines_runtime_compiled64_9.dll", "cudnn_heuristic64_9.dll",
    "cudnn_ops64_9.dll", "cudnn_adv64_9.dll", "cudnn_cnn64_9.dll",
    "cudnn_ext64_9.dll", "cudnn_engines_tensor_ir64_9.dll"
)

$root = Join-Path $env:TEMP ("pb-remote-install-test-" + [guid]::NewGuid().ToString("N"))
$serve = Join-Path $root "serve"
$bundle = Join-Path $root "bundle"
$install = Join-Path $root "install"
New-Item -ItemType Directory -Path (Join-Path $bundle "models") -Force | Out-Null
New-Item -ItemType Directory -Path $serve -Force | Out-Null

Set-Content -LiteralPath (Join-Path $bundle "pb_human_parser_cuda.dll") -Value "dummy-runtime" -NoNewline
Set-Content -LiteralPath (Join-Path $bundle "models\fashn-human-parser.onnx") -Value "dummy-model" -NoNewline
Set-Content -LiteralPath (Join-Path $bundle "models\manifest.json") -Value '{"model":"dummy"}' -NoNewline
foreach ($name in $closure) {
    Set-Content -LiteralPath (Join-Path $bundle $name) -Value "dummy" -NoNewline
}

Compress-Archive -Path (Join-Path $bundle "*") -DestinationPath (Join-Path $serve "bundle.zip") -Force

$files = @{}
foreach ($relative in @("pb_human_parser_cuda.dll", "models/fashn-human-parser.onnx", "models/manifest.json")) {
    $path = Join-Path $bundle ($relative -replace "/", "\")
    $files[$relative] = @{
        sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$bundleHash = (Get-FileHash -LiteralPath (Join-Path $serve "bundle.zip") -Algorithm SHA256).Hash.ToLowerInvariant()
$manifest = @{
    bundle = @{
        url = "http://127.0.0.1:$Port/bundle.zip"
        sha256 = $bundleHash
    }
    files = $files
} | ConvertTo-Json -Depth 6
Set-Content -LiteralPath (Join-Path $serve "manifest.json") -Value $manifest -Encoding utf8
$manifestHash = (Get-FileHash -LiteralPath (Join-Path $serve "manifest.json") -Algorithm SHA256).Hash.ToLowerInvariant()

$server = Start-Process -FilePath $python `
    -ArgumentList @("-m", "http.server", "$Port", "--bind", "127.0.0.1") `
    -WorkingDirectory $serve -PassThru -WindowStyle Hidden

try {
    $ready = $false
    for ($i = 0; $i -lt 50; $i++) {
        try {
            $client = New-Object System.Net.Sockets.TcpClient
            $client.Connect("127.0.0.1", $Port)
            $client.Close()
            $ready = $true
            break
        } catch {
            Start-Sleep -Milliseconds 100
        }
    }
    if (-not $ready) { throw "local HTTP server did not become ready on port $Port" }

    $wrongOutput = & $pwsh -NoProfile -File $installer `
        -ManifestUrl "http://127.0.0.1:$Port/manifest.json" `
        -ManifestSha256 ("0" * 64) -InstallDir $install -Force 2>&1 | Out-String
    $wrongExit = $LASTEXITCODE
    Write-Output $wrongOutput
    if ($wrongExit -eq 0) { throw "a wrong manifest pin unexpectedly succeeded" }
    if (Test-Path -LiteralPath (Join-Path $install "pb_human_parser_cuda.dll")) {
        throw "a wrong manifest pin wrote an install"
    }

    $rightOutput = & $pwsh -NoProfile -File $installer `
        -ManifestUrl "http://127.0.0.1:$Port/manifest.json" `
        -ManifestSha256 $manifestHash -InstallDir $install -Force 2>&1 | Out-String
    $rightExit = $LASTEXITCODE
    Write-Output $rightOutput
    if ($rightExit -ne 0) { throw "the pinned remote install failed" }

    $recordPath = Join-Path $install "installed.json"
    if (-not (Test-Path -LiteralPath $recordPath)) { throw "installed.json was not written" }
    $record = Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json
    if ($record.closure.Count -ne $closure.Count) {
        throw "closure count $($record.closure.Count) != $($closure.Count)"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $install "models\fashn-human-parser.onnx"))) {
        throw "model was not installed"
    }
    Write-Output ("REMOTE-INSTALL|PASS|wrong_exit={0}|right_exit={1}|closure={2}" -f $wrongExit, $rightExit, $record.closure.Count)
}
finally {
    $serverProcess = Get-Process -Id $server.Id -ErrorAction SilentlyContinue
    if ($serverProcess) { Stop-Process -Id $serverProcess.Id -Force }
    if (-not $Keep) { Remove-Item -LiteralPath $root -Recurse -Force }
}
