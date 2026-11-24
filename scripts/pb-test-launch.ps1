# pb-test-launch.ps1 — launch the real NAI Paintbrush with the test hook enabled
#
#   pwsh scripts/pb-test-launch.ps1 -Media <image> -SamModel <ggml> `
#        -SamDevice gpu -LogFile <log> -EndpointFile <json> [-LamaDevice cpu]
#
# It starts scripts/launch-pb-noskip.ps1 in a hidden helper console (the app
# window itself is normal), waits for the in-app hook endpoint, verifies it with
# `hello`, and prints a JSON run record: { pid, launcherPid, endpointFile }.
# No desktop input or foreground manipulation is used.

param(
    [Parameter(Mandatory = $true)][string]$Media,
    [Parameter(Mandatory = $true)][string]$SamModel,
    [string]$SamDevice = 'gpu',
    [Parameter(Mandatory = $true)][string]$LogFile,
    [Parameter(Mandatory = $true)][string]$EndpointFile,
    [ValidateSet('', 'gpu', 'cpu')][string]$LamaDevice = '',
    [int]$TimeoutSec = 120
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'pb-hook-client.ps1')

$launch = Join-Path $PSScriptRoot 'launch-pb-noskip.ps1'
if (-not (Test-Path -LiteralPath $launch)) {
    throw "Launch script not found: $launch"
}
if (Test-Path -LiteralPath $EndpointFile) {
    Remove-Item -LiteralPath $EndpointFile -Force
}

$env:PB_TEST_HOOK = '1'
$env:PB_TEST_HOOK_FILE = $EndpointFile
if ($LamaDevice) {
    $env:PB_LAMA_DEVICE = $LamaDevice
} else {
    Remove-Item Env:PB_LAMA_DEVICE -ErrorAction SilentlyContinue
}

$pwsh = (Get-Process -Id $PID).Path
$arguments = @(
    '-NoLogo', '-NoProfile', '-NonInteractive',
    '-File', $launch,
    '-Media', $Media,
    '-NoAutosave',
    '-SamModel', $SamModel,
    '-SamDevice', $SamDevice,
    '-LogFile', $LogFile
)
$launcher = Start-Process -FilePath $pwsh -ArgumentList $arguments `
    -WindowStyle Hidden -PassThru

$appPid = 0
$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
while ([DateTime]::UtcNow -lt $deadline) {
    if ($launcher.HasExited) {
        throw "Launch helper exited before nai-paintbrush started (code $($launcher.ExitCode))."
    }
    $child = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($launcher.Id)" |
        Where-Object { $_.Name -eq 'nai-paintbrush.exe' } |
        Select-Object -First 1
    if ($child) {
        $appPid = [int]$child.ProcessId
        break
    }
    Start-Sleep -Milliseconds 250
}
if (-not $appPid) {
    Stop-Process -Id $launcher.Id -Force -ErrorAction SilentlyContinue
    throw "nai-paintbrush.exe did not start within $TimeoutSec s."
}

$endpoint = Wait-PbHookEndpoint -Path $EndpointFile -TimeoutSec $TimeoutSec
$hello = Invoke-PbHook -Endpoint $endpoint -Command 'hello'
$record = [pscustomobject]@{
    pid          = $appPid
    launcherPid  = $launcher.Id
    endpointFile = (Resolve-Path -LiteralPath $EndpointFile).Path
    port         = $endpoint.port
    startedUtc   = [DateTime]::UtcNow.ToString('o')
    hello        = $hello
}
$record | ConvertTo-Json -Depth 10
