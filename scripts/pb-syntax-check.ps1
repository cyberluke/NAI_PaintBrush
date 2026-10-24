# Syntax-check NAI Paintbrush C++ sources against Qt6 (msys2 mingw64) with the
# harness stub vlc_common.h. No VLC build is involved. Prints every diagnostic
# and exits non-zero on the first failing file.
#
#   pwsh scripts/pb-syntax-check.ps1 [file ...]
#
# With no arguments it checks every .cpp under modules/gui/qt/paintbrush/art
# plus the top-level paintbrush sources that the art runtime touches.
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Files)

$ErrorActionPreference = 'Continue'
$qt = 'C:\msys64\mingw64'
$env:PATH = "$qt\bin;" + $env:PATH   # cc1plus needs the mingw DLLs on PATH
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$pb = Join-Path $root 'modules\gui\qt\paintbrush'
$stub = Join-Path $root '_REVIEW\harness\pb_art_harness\stub'

if (-not $Files -or $Files.Count -eq 0) {
    $Files = @(Get-ChildItem -Recurse (Join-Path $pb 'art') -Filter *.cpp | ForEach-Object { $_.FullName })
    $Files += @(Get-ChildItem $pb -Filter *.cpp | ForEach-Object { $_.FullName })
}

$inc = @('-I', $stub, '-I', $pb, '-I', (Join-Path $root 'modules\gui\qt'),
         '-I', (Join-Path $root 'include'),
         '-I', "$qt\include\qt6", '-I', "$qt\include\qt6\QtCore",
         '-I', "$qt\include\qt6\QtGui", '-I', "$qt\include\qt6\QtNetwork",
         '-I', "$qt\include\qt6\QtQuick", '-I', "$qt\include\qt6\QtQml",
         '-I', "$qt\include\qt6\QtQmlIntegration")

$failed = 0
foreach ($f in $Files) {
    $rel = $f.Replace($root + '\', '')
    $out = & "$qt\bin\g++.exe" -std=c++17 -fsyntax-only -Wall -Wextra -Wno-missing-field-initializers `
        @inc $f 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        $failed++
        Write-Output "FAIL $rel"
        $out | Select-Object -First 40 | ForEach-Object { Write-Output "    $_" }
    } else {
        $warn = @($out | Where-Object { $_ -match 'warning:' }).Count
        Write-Output "ok   $rel (warnings: $warn)"
        if ($warn -gt 0) { $out | Where-Object { $_ -match 'warning:' } | Select-Object -First 6 | ForEach-Object { Write-Output "    $_" } }
    }
}
Write-Output "---- $failed failing file(s)"
exit $failed
