@echo off
REM Same as Install OmniAI.bat, but computer ALLIES also play as OmniAI.
REM The plain installer only changes computer opponents.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_omniai.ps1" -Allied %*
echo.
pause
