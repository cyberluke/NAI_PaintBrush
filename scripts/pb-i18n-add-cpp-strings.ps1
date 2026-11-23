# Merge the PaintBrush C++ status strings into the seven catalogs.
#
# The strings live in scripts/pb-i18n-paintbrush-cpp.json (msgid -> per-language
# translation); this script appends only the msgids that are not yet present,
# so it is idempotent and safe to run before every catalog compile.
#
#   pwsh scripts/pb-i18n-add-cpp-strings.ps1
#
# Followed by scripts/pb-i18n-compile.ps1 (or scripts/pb-build-plugin.ps1).

[CmdletBinding()]
param(
    [string]$Root = (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
)

$ErrorActionPreference = "Stop"
$tablePath = Join-Path $Root 'scripts\pb-i18n-paintbrush-cpp.json'
$table = Get-Content -LiteralPath $tablePath -Raw -Encoding UTF8 | ConvertFrom-Json
$languages = @('cs', 'sk', 'vi', 'fr', 'de', 'it', 'es')
$header = "# NAI PaintBrush C++ status strings (Settings capability rows)"

foreach ($lang in $languages) {
    $poPath = Join-Path $Root ("po\" + $lang + ".po")
    $text = [IO.File]::ReadAllText($poPath, [Text.Encoding]::UTF8)
    $appended = 0
    $blocks = New-Object System.Collections.Generic.List[string]

    foreach ($entry in $table.PSObject.Properties) {
        $msgid = $entry.Name
        if ($text.Contains('msgid "' + $msgid + '"')) { continue }
        $msgstr = $entry.Value.$lang
        if (-not $msgstr) { throw "Missing $lang translation for: $msgid" }
        $blocks.Add("msgid `"$msgid`"`nmsgstr `"$msgstr`"")
        $appended++
    }

    if ($appended -eq 0) {
        Write-Output ("{0}: up to date" -f $lang)
        continue
    }

    if (-not $text.EndsWith("`n")) { $text += "`n" }
    $text += "`n" + $header + "`n`n" + ($blocks -join "`n`n") + "`n"
    [IO.File]::WriteAllText($poPath, $text, (New-Object Text.UTF8Encoding($false)))
    Write-Output ("{0}: appended {1} msgids" -f $lang, $appended)
}
