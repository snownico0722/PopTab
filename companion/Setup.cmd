@echo off
setlocal
cd /d "%~dp0"
echo PopTab Companion - automatic setup
echo ----------------------------------
echo Detecting PopTab in Chrome, Edge and Brave...
echo.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
set "setup_result=%ERRORLEVEL%"
echo.
if not "%setup_result%"=="0" (
  echo Setup was not completed. See the message above.
  echo Load the PopTab extension in your browser, then try again.
) else (
  echo Setup finished. Open a PopTab popup to use the titlebar pin.
)
echo.
pause
exit /b %setup_result%
