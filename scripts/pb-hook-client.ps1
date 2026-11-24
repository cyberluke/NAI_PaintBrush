# pb-hook-client.ps1 — client for the NAI Paintbrush test-only loopback hook.
#
# The app publishes {pid, port, token} to the file named by PB_TEST_HOOK_FILE
# (or %TEMP%\nai-paintbrush-test-<pid>.json). Dot-source this file and use:
#
#   $ep = Wait-PbHookEndpoint -Path $endpointFile -TimeoutSec 90
#   Invoke-PbHook -Endpoint $ep -Command 'hello'
#   Invoke-PbHook -Endpoint $ep -Command 'set_selection_rect' -Args @{x=10;y=10;w=80;h=80}
#
# No desktop input, cursor movement or foreground stealing is involved: the
# hook runs inside the real application and calls the real controller/canvas
# entry points. Requests are newline-delimited JSON over 127.0.0.1 with a
# per-process token.

function Wait-PbHookEndpoint {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [int]$TimeoutSec = 90
    )
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path -LiteralPath $Path) {
            try {
                $raw = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
                if ($raw -and $raw.Trim().Length -gt 0) {
                    return ($raw | ConvertFrom-Json)
                }
            } catch {
                # the app may be mid-write; retry until the deadline
            }
        }
        Start-Sleep -Milliseconds 200
    }
    throw "Paintbrush hook endpoint not available: $Path"
}

function Invoke-PbHook {
    param(
        [Parameter(Mandatory = $true)]$Endpoint,
        [Parameter(Mandatory = $true)][string]$Command,
        [hashtable]$HookArgs = @{},
        [int]$Id = 0
    )
    $payload = @{
        token = [string]$Endpoint.token
        cmd   = $Command
        args  = $HookArgs
        id    = $Id
    } | ConvertTo-Json -Depth 12 -Compress

    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $client.Connect('127.0.0.1', [int]$Endpoint.port)
        $stream = $client.GetStream()
        $utf8 = [System.Text.UTF8Encoding]::new($false)
        $writer = [System.IO.StreamWriter]::new($stream, $utf8)
        $reader = [System.IO.StreamReader]::new($stream, $utf8)
        $writer.NewLine = "`n"
        $writer.WriteLine($payload)
        $writer.Flush()
        $line = $reader.ReadLine()
    } finally {
        $client.Close()
    }
    if (-not $line) {
        throw "Paintbrush hook closed the connection for command '$Command'"
    }
    $response = $line | ConvertFrom-Json
    if (-not $response.ok) {
        throw "Paintbrush hook command '$Command' failed: $($response.error)"
    }
    return $response
}
