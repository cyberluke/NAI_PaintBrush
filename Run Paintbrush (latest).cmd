@echo off
rem ============================================================
rem  Run Paintbrush (latest build) - convenience launcher
rem  Double-click me, or run from a terminal.
rem  Launches the latest win64-meson build via the project's
rem  canonical launch script (paintbrush.exe, no decoder skip).
rem ============================================================
set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"

rem the project launch script lives in scripts/
if not exist "%ROOT%\scripts\launch-pb-noskip.ps1" (
    echo ERROR: launch-pb-noskip.ps1 not found under "%ROOT%\scripts"
    pause
    exit /b 1
)

rem prefer a real terminal so Ctrl-C / logs behave
start "NAI Paintbrush" /D "%ROOT%" powershell -NoExit -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\scripts\launch-pb-noskip.ps1"