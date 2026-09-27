@echo off
REM Removes OmniAI and puts the AI selection back to Nullkiller.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_omniai.ps1" -Uninstall %*
echo.
pause
