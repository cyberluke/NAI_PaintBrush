@echo off
rem ============================================================
rem  Push to Git (master) - convenience launcher
rem  Stages all changes, commits with a timestamped message,
rem  and pushes the current branch to origin.  Shows result and
rem  keeps the window open so you can read the output.
rem ============================================================
set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "COMMIT_MSG=[push] %DATE% %TIME%"

pushd "%ROOT%" || (echo ERROR: bad root "%ROOT%" & pause & exit /b 1)

echo === git status ==================================================
git status --short

echo.
echo === staging all changes =========================================
git add -A
git --no-pager diff --cached --stat

echo.
echo === committing ==================================================
git commit -m "%COMMIT_MSG%"
set "CMDRC=%ERRORLEVEL%"

echo.
echo === pushing to origin ==========================================
git push origin master
set "PUSHRC=%ERRORLEVEL%"

popd

echo.
if "%CMDRC%"=="0" if "%PUSHRC%"=="0" (
    echo OK: changes committed and pushed.
) else (
    echo DONE with warnings/errors above ^(rc commit=%CMDRC%, push=%PUSHRC%^).
)
pause