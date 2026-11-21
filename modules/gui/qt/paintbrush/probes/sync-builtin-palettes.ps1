<#
.SYNOPSIS
  Regenerate the embedded built-in palette literal in pb_palette_store.cpp
  from palettes/builtin-palettes.json.

.DESCRIPTION
  The JSON file is the source of truth for shipped palettes. The C++ copy
  exists so the store works with no runtime file dependency, but a hand-kept
  duplicate WILL drift. This script rewrites the R"JSON( ... )JSON" block in
  place, so adding a palette is a one-file edit plus this command.

  Run after editing the JSON, then rebuild.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$jsonPath = Join-Path $root "palettes/builtin-palettes.json"
$cppPath = Join-Path $root "pb_palette_store.cpp"

if (-not (Test-Path -LiteralPath $jsonPath)) { throw "missing $jsonPath" }
if (-not (Test-Path -LiteralPath $cppPath)) { throw "missing $cppPath" }

# Validate + normalise the JSON first: the embedded copy must be valid JSON.
$doc = Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json
$compact = $doc | ConvertTo-Json -Depth 8 -Compress:$false
# ConvertTo-Json renders the schema key fine; keep the exact key name.

$cpp = Get-Content -LiteralPath $cppPath -Raw
$pattern = '(?s)R"JSON\(.*?\)JSON"'
if ($cpp -notmatch $pattern) { throw "embedded palette block not found in $cppPath" }

$replacement = 'R"JSON(' + "`n" + $compact + "`n" + ')JSON"'
$updated = [regex]::Replace($cpp, $pattern, $replacement.Replace('$', '$$'))
Set-Content -LiteralPath $cppPath -Value $updated -NoNewline -Encoding utf8

Write-Output "synced $($doc.palettes.Count) palettes into pb_palette_store.cpp"
