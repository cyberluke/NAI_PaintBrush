<#
.SYNOPSIS
  NAI Paintbrush — static QML contract checks.

.DESCRIPTION
  Two bug classes bit us repeatedly and both are statically detectable:

   1. `model.<role>` inside a delegate. Paintbrush passes QVariantList models
      (thumbnailList, trackList, harvest…), where the delegate data is
      `modelData.<role>`; `model.<role>` silently evaluates to undefined.
      Symptom: empty boxes, no thumbnails, dead click targets.

   2. Binding `text:` / `description:` / `placeholderText:` to a
      `PaintbrushCtx.<name>` that is not declared as a Q_PROPERTY or
      Q_INVOKABLE. Symptom: empty labels or "[object Object]".

  Run from the probe runner or CI. Exit 1 on any violation.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$qmlDir = Join-Path $root "qml"
$hpp = Join-Path $root "pb_controller.hpp"

$violations = New-Object System.Collections.Generic.List[string]

# ---- 1. model.<role> misuse ------------------------------------------------
Get-ChildItem -LiteralPath $qmlDir -Filter *.qml | ForEach-Object {
    $lines = Get-Content -LiteralPath $_.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        if ($line -match '^\s*(//|\*|/\*)') { continue }
        # ignore comments on the same line
        $code = ($line -split '//')[0]
        if ($code -match '(?<![\w.])model\.[A-Za-z_]') {
            $violations.Add("$($_.Name):$($i + 1): model.<role> — use modelData.<role> :: $($line.Trim())")
        }
    }
}

# ---- 2. PaintbrushCtx.<name> must be declared ------------------------------
$declared = @{}
if (Test-Path -LiteralPath $hpp) {
    $hppText = Get-Content -LiteralPath $hpp -Raw
    [regex]::Matches($hppText, 'Q_PROPERTY\([^)]*?\b(\w+)\s+READ\b') | ForEach-Object {
        $declared[$_.Groups[1].Value] = $true
    }
    [regex]::Matches($hppText, 'Q_INVOKABLE\s+[^;\r\n]*?\b(\w+)\s*\(') | ForEach-Object {
        $declared[$_.Groups[1].Value] = $true
    }
}

$bind = '(?:text|description|placeholderText|title)\s*:\s*.*?PaintbrushCtx\.([A-Za-z_]\w*)'
Get-ChildItem -LiteralPath $qmlDir -Filter *.qml | ForEach-Object {
    $lines = Get-Content -LiteralPath $_.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        if ($line -match '^\s*(//|\*|/\*)') { continue }
        $code = ($line -split '//')[0]
        foreach ($m in [regex]::Matches($code, $bind)) {
            $name = $m.Groups[1].Value
            if (-not $declared.ContainsKey($name)) {
                $violations.Add("$($_.Name):$($i + 1): PaintbrushCtx.$name is not a declared property/invokable :: $($line.Trim())")
            }
        }
    }
}

if ($violations.Count -eq 0) {
    Write-Output "QML contracts: PASS ($($declared.Count) controller members declared)"
    exit 0
}

Write-Output "QML contracts: FAIL ($($violations.Count) violation(s))"
$violations | ForEach-Object { Write-Output "  $_" }
exit 1
