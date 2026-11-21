<#
.SYNOPSIS
  NAI Paintbrush — isolated QML probe runner (the Qt equivalent of a
  Storybook test run).

.DESCRIPTION
  Runs every probes/qml/Probe*.qml story in a real Qt Quick runtime
  (qml.exe), collects its RESULT|ASSERT lines and fails the run if any
  assertion fails or a probe crashes.

  Why this exists: the Paintbrush UI is composed of QML components whose
  contracts (model accessor shapes, delegate hit-testing, visibility
  bindings) cannot be verified by compiling. A probe pins one contract,
  runs in ~1s with no VLC GUI, and prints machine-checkable PASS/FAIL.

  Add a probe: drop Probe<Contract>.qml in probes/qml, assert with
  expect(name, actual, wanted), exit non-zero on failure. Nothing else.

.EXAMPLE
  pwsh -File modules/gui/qt/paintbrush/probes/run-probes.ps1
  pwsh -File modules/gui/qt/paintbrush/probes/run-probes.ps1 -Probe ProbeModelBinding
#>
[CmdletBinding()]
param(
    [string]$QtBin = "C:\msys64\mingw64\bin",
    [string]$Probe
)

$ErrorActionPreference = "Stop"

$probeDir = Join-Path $PSScriptRoot "qml"
if (-not (Test-Path -LiteralPath $probeDir)) {
    Write-Error "No probe directory at $probeDir"
    exit 2
}

$qmlExe = Join-Path $QtBin "qml.exe"
if (-not (Test-Path -LiteralPath $qmlExe)) {
    Write-Error "qml.exe not found at $qmlExe (pass -QtBin)"
    exit 2
}

# Qt on Windows is a GUI-subsystem binary: without this, qml: console lines
# never reach the console and every probe would look silent-and-green.
$env:QT_ASSUME_STDERR_HAS_CONSOLE = "1"
$env:PATH = "$QtBin;C:\msys64\usr\bin;$env:PATH"

$files = Get-ChildItem -LiteralPath $probeDir -Filter "Probe*.qml" | Sort-Object Name
if ($Probe) {
    $files = $files | Where-Object { $_.BaseName -eq $Probe }
}
if (-not $files) {
    Write-Error "No probes matched in $probeDir"
    exit 2
}

$failed = 0
$results = New-Object System.Collections.Generic.List[string]

# ---- static contract checks (model roles, undeclared PaintbrushCtx bindings)
$contracts = Join-Path $PSScriptRoot "check-qml-contracts.ps1"
if (Test-Path -LiteralPath $contracts) {
    $cOut = & $contracts 2>&1 | Out-String
    $cCode = $LASTEXITCODE
    if ($cCode -ne 0) { $failed++ }
    $results.Add(("check-qml-contracts          {0}" -f $(if ($cCode -eq 0) { "PASS" } else { "FAIL" })))
    foreach ($l in ($cOut -split "`r?`n" | Where-Object { $_.Trim() })) {
        $results.Add("    " + $l.Trim())
    }
    $results.Add("")
}

# ---- built-in palette data contract ---------------------------------------
$palettes = Join-Path $PSScriptRoot "check-palettes.ps1"
if (Test-Path -LiteralPath $palettes) {
    $pOut = & $palettes 2>&1 | Out-String
    $pCode = $LASTEXITCODE
    if ($pCode -ne 0) { $failed++ }
    $results.Add(("check-palettes               {0}" -f $(if ($pCode -eq 0) { "PASS" } else { "FAIL" })))
    foreach ($l in ($pOut -split "`r?`n" | Where-Object { $_.Trim() })) {
        $results.Add("    " + $l.Trim())
    }
    $results.Add("")
}

foreach ($f in $files) {
    $name = $f.BaseName
    $raw = & $qmlExe $f.FullName 2>&1 | Out-String
    $code = $LASTEXITCODE

    $lines = ($raw -split "`r?`n") | Where-Object { $_ -match "RESULT\|" } |
        ForEach-Object { ($_ -replace "^.*?RESULT\|", "") }

    $asserts = $lines | Where-Object { $_ -like "ASSERT.*" }
    $verdict = $lines | Where-Object { $_ -like "VERDICT|*" } | Select-Object -Last 1

    $bad = @($asserts | Where-Object { $_ -match "\|FAIL" })
    $probeOk = ($code -eq 0) -and ($bad.Count -eq 0) -and ($verdict -match "PASS")

    if (-not $probeOk) { $failed++ }

    $status = if ($probeOk) { "PASS" } else { "FAIL" }
    $results.Add(("{0,-28} {1}" -f $name, $status))

    foreach ($a in $asserts) { $results.Add("    " + $a) }
    if ($code -ne 0 -and $verdict -notmatch "PASS") {
        $results.Add("    exit=$code")
        # surface the first real Qt error, not the whole plugin dump
        $err = ($raw -split "`r?`n") | Where-Object { $_ -match "^qml: |error|Error" } |
            Select-Object -First 3
        foreach ($e in $err) { $results.Add("    " + $e.Trim()) }
    }
    $results.Add("")
}

$results | ForEach-Object { Write-Output $_ }

$total = @($files).Count
Write-Output ("{0}/{1} probes passed" -f ($total - $failed), $total)
exit ([int]($failed -gt 0))
