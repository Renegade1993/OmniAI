@echo off
REM Configure + build OmniAI under a VS dev environment (vcvars64 gives
REM cl/link/lib plus INCLUDE/LIB for MSVC + WinSDK).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
REM ninja: %NINJA% if set, else the one on PATH, else winget's install.
if not defined NINJA for /f "delims=" %%n in ('where ninja 2^>nul') do if not defined NINJA set "NINJA=%%n"
if not defined NINJA set "NINJA=%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe\ninja.exe"
cd /d "%~dp0"
if not exist build mkdir build
cd build
"C:\Program Files\CMake\bin\cmake.exe" -G Ninja ^
	-DCMAKE_MAKE_PROGRAM="%NINJA%" ^
	-DCMAKE_BUILD_TYPE=Release ^
	..
if errorlevel 1 (echo CONFIGURE FAILED & exit /b 1)
"%NINJA%"
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo BUILD OK
