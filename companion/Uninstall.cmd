@echo off
setlocal
cd /d "%~dp0"
echo This removes PopTab Companion for the current Windows user.
choice /M "Uninstall"
if errorlevel 2 exit /b 0
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" -Browser All
set "result=%ERRORLEVEL%"
echo.
pause
exit /b %result%
