# Build the Qt plugin (libqt_plugin.dll) with the patched win64-meson graph and
# regenerate the plugin cache. Prints only warnings/errors + the exit code.
$env:PATH = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:PATH
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location (Join-Path $root 'win64-meson')
$log = Join-Path $root 'win64-meson\pb-build.log'
& ninja modules/libqt_plugin.dll 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $log |
    Where-Object { (($_ -notmatch '^\[\d+/\d+\]') -or ($_ -match 'FAILED|error')) -and
                   ($_ -notmatch "_WIN32_WINNT' redefined|WINVER' redefined|location of the previous definition|No relevant classes found") }
$code = $LASTEXITCODE
if ($code -eq 0) {
    $env:PATH = (Join-Path $root 'win64-meson\bin') + ';' + (Join-Path $root 'win64-meson\lib') + ';' + (Join-Path $root 'win64-meson\src') + ';' + $env:PATH
    & (Join-Path $root 'win64-meson\bin\vlc-cache-gen.exe') (Join-Path $root 'win64-meson\modules') 2>&1 | ForEach-Object { "$_" } | Select-Object -Last 2
    Write-Output ("plugins.dat: " + (Get-Item (Join-Path $root 'win64-meson\modules\plugins.dat')).LastWriteTime)
}
Write-Output "NINJA_EXIT=$code"
exit $code
