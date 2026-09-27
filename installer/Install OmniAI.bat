@echo off
REM Double-click me. Close VCMI first.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_omniai.ps1" %*
echo.
pause
