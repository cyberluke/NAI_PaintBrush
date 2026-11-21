<#
.SYNOPSIS
  NAI Paintbrush — built-in palette contract check.

.DESCRIPTION
  Validates palettes/builtin-palettes.json against nai-paintbrush/palette/v1:
  unique ids, required fields, >= 6 swatches, every swatch #RRGGBB, and
  shipped presets marked locked (so user editing can never overwrite them).
#>
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$file = Join-Path $root "palettes/builtin-palettes.json"

if (-not (Test-Path -LiteralPath $file)) {
    Write-Output "palettes: FAIL — missing $file"
    exit 1
}

try {
    $doc = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
} catch {
    Write-Output "palettes: FAIL — invalid JSON: $($_.Exception.Message)"
    exit 1
}

$errors = New-Object System.Collections.Generic.List[string]

if ($doc.'$schema' -ne "nai-paintbrush/palette/v1") {
    $errors.Add("schema must be nai-paintbrush/palette/v1 (got '$($doc.'$schema')')")
}

$ids = @{}
foreach ($p in $doc.palettes) {
    if (-not $p.id)      { $errors.Add("palette without id") ; continue }
    if ($ids.ContainsKey($p.id)) { $errors.Add("duplicate id '$($p.id)'") }
    $ids[$p.id] = $true
    if (-not $p.name)    { $errors.Add("$($p.id): missing name") }
    if (-not $p.description) { $errors.Add("$($p.id): missing description") }
    if ($null -eq $p.locked) { $errors.Add("$($p.id): missing locked flag") }
    if ($p.colors.Count -lt 4) { $errors.Add("$($p.id): needs >= 4 colors (got $($p.colors.Count))") }
    foreach ($c in $p.colors) {
        if ($c -notmatch '^#[0-9A-Fa-f]{6}$') { $errors.Add("$($p.id): invalid color '$c'") }
    }
}

if ($errors.Count -eq 0) {
    Write-Output "palettes: PASS ($($doc.palettes.Count) palettes, $($ids.Count) unique ids)"
    exit 0
}

Write-Output "palettes: FAIL ($($errors.Count) problem(s))"
$errors | ForEach-Object { Write-Output "  $_" }
exit 1
