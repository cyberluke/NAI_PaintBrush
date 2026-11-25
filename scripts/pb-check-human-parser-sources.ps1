<#
.SYNOPSIS
  Compile-check the new MinGW human parser sources without building the plugin.

.DESCRIPTION
  Reuses the exact compile flags from win64-meson/compile_commands.json (the
  same Qt/VLC include paths, defines and warnings as pb_matting_service.cpp)
  and runs the MinGW compiler with -fsyntax-only for each new source.

  This is deliberately NOT a plugin build: the worktree's win64-meson tree is
  shared with other agents. The MSVC runtime DLL is built separately by
  scripts/pb-build-human-parser.ps1.
#>
[CmdletBinding()]
param(
    [string]$BuildDir = "",
    [string]$Compiler = "C:\msys64\mingw64\bin\g++.exe"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $repo "win64-meson"
}
$dbPath = Join-Path $BuildDir "compile_commands.json"
if (-not (Test-Path -LiteralPath $dbPath)) {
    throw "compile_commands.json missing: $dbPath"
}
if (-not (Test-Path -LiteralPath $Compiler)) {
    throw "MinGW compiler missing: $Compiler"
}
# cc1plus lives under libexec and resolves its DLL closure from the MinGW bin
# directory. The live meson build gets this from the MSYS2 environment; an
# agent shell must add it explicitly or cc1plus exits silently with code 1.
$mingwBin = Split-Path -Parent $Compiler
$env:PATH = "$mingwBin;$env:PATH"

$db = Get-Content -LiteralPath $dbPath -Raw | ConvertFrom-Json
$entry = $db | Where-Object {
    ($_.file -replace '\\', '/') -match 'paintbrush/pb_matting_service\.cpp$'
} | Select-Object -First 1
if (-not $entry) {
    throw "no pb_matting_service.cpp entry in $dbPath"
}

$command = [string]$entry.command
$md = $command.IndexOf(' -MD')
if ($md -lt 0) {
    throw "unexpected compile command shape (no -MD)"
}
$prefix = $command.Substring(0, $md)
$cxx = $prefix.IndexOf('"c++"')
if ($cxx -lt 0) {
    throw "unexpected compile command shape (no c++ driver)"
}
$rawArgs = $prefix.Substring($cxx + 5).Trim()

$sources = @(
    "../modules/gui/qt/paintbrush/pb_human_parser_service.cpp",
    "../modules/gui/qt/paintbrush/gpu/pb_human_parser_shim.cpp"
)

foreach ($source in $sources) {
    Write-Host "syntax check: $source"
    # Pass the meson argument string verbatim through CreateProcess. Parsing it
    # through PowerShell would re-interpret the escaped -DGETTEXT_PACKAGE and
    # PB_BUILD_SHA quotes and silently corrupt the command.
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $Compiler
    $psi.Arguments = "$rawArgs -fsyntax-only `"$source`""
    $psi.WorkingDirectory = $BuildDir
    $psi.UseShellExecute = $false
    $process = [System.Diagnostics.Process]::Start($psi)
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        throw "syntax check failed: $source (exit $($process.ExitCode))"
    }
}
Write-Host "SYNTAX PASS"
