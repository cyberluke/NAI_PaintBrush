# Compile the NAI Paintbrush gettext catalogs into the runtime locale trees.
# VLC on Windows resolves the locale directory from the libvlccore base (the
# build's src/) and from the executable directory (bin/), so both trees are
# refreshed. The language set has one owner: scripts/pb-i18n-languages.json.
# Called by pb-build-plugin.ps1 after every successful build and by
# pb-build-msix.ps1 before staging, so a packaged app always ships fresh .mo
# files.
[CmdletBinding()]
param(
    [string]$Root = (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
)

$ErrorActionPreference = 'Stop'

$languageFile = Join-Path $Root 'scripts\pb-i18n-languages.json'
if (-not (Test-Path -LiteralPath $languageFile)) {
    throw "Language list not found: $languageFile"
}
$languages = @((Get-Content -Raw -LiteralPath $languageFile | ConvertFrom-Json).PSObject.Properties.Name)

# msgfmt is provided by the MSYS2 gettext package. Prefer PATH (pb-build-plugin.ps1
# prepends both mingw64\bin and usr\bin), then the standard MSYS2 locations.
$msgfmt = (Get-Command 'msgfmt.exe' -ErrorAction SilentlyContinue).Source
if (-not $msgfmt) {
    $msgfmt = @('C:\msys64\usr\bin\msgfmt.exe', 'C:\msys64\mingw64\bin\msgfmt.exe') |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $msgfmt) {
    throw "msgfmt not found. Install the MSYS2 gettext package (pacman -S gettext) or add msgfmt.exe to PATH."
}

$build = Join-Path $Root 'win64-meson'
# Compile into a staging tree first: a failed catalog aborts before any runtime
# tree is touched, so the app can never see a mixed .mo set.
$staging = Join-Path $build 'pb-locale-staging'
if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
foreach ($lang in $languages) {
    $po = Join-Path $Root "po\$lang.po"
    if (-not (Test-Path -LiteralPath $po)) { throw "Catalog not found: $po" }
    $dir = Join-Path $staging "$lang\LC_MESSAGES"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    & $msgfmt -o (Join-Path $dir 'vlc.mo') $po
    if ($LASTEXITCODE -ne 0) { throw "msgfmt failed for $lang -> $po" }
}

$targets = @(
    (Join-Path $build 'src\locale'),
    (Join-Path $build 'bin\locale')
)
foreach ($target in $targets) {
    foreach ($lang in $languages) {
        $dir = Join-Path $target "$lang\LC_MESSAGES"
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        Copy-Item -LiteralPath (Join-Path $staging "$lang\LC_MESSAGES\vlc.mo") `
            -Destination (Join-Path $dir 'vlc.mo') -Force
    }
}
Remove-Item -LiteralPath $staging -Recurse -Force
Write-Output ("LOCALE_MO: {0} catalogs -> {1}" -f $languages.Count, ($targets -join '; '))
