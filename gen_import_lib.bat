@echo off
REM Generate VCMI_lib import library from the installed facade DLL.
REM Needs a VS dev environment (vcvars64) for dumpbin.exe and lib.exe.
REM VCMI_BIN names a VCMI install folder (the one holding VCMI_lib.dll); it is only read.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if "%VCMI_BIN%"=="" (echo Set VCMI_BIN to a VCMI install folder, the one holding VCMI_lib.dll & exit /b 2)
set "DLL=%VCMI_BIN%\VCMI_lib.dll"
set "OUTDIR=%~dp0..\resources"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
echo Dumping exports...
dumpbin /NOLOGO /EXPORTS "%DLL%" > "%OUTDIR%\vcmi_exports.txt"
if errorlevel 1 (echo dumpbin failed & exit /b 1)
echo Building .def...
powershell -NoProfile -Command "$lines = Get-Content '%OUTDIR%\vcmi_exports.txt'; $out = @('LIBRARY VCMI_lib','EXPORTS'); $inExp = $false; foreach ($l in $lines) { if ($l -match 'ordinal +hint') { $inExp = $true; continue }; if ($inExp) { $t = $l.Trim(); if ($t -eq '') { continue }; if ($t -match '^Summary') { break }; if ($t -match '\s(\S+)\s*$') { $out += '  ' + $Matches[1] } } }; Set-Content '%OUTDIR%\VCMI_lib.def' $out -Encoding ASCII"
if errorlevel 1 (echo def build failed & exit /b 1)
echo Creating import library...
lib /DEF:"%OUTDIR%\VCMI_lib.def" /OUT:"%OUTDIR%\VCMI_lib.lib" /MACHINE:X64
if errorlevel 1 (echo lib failed & exit /b 1)
echo Done: %OUTDIR%\VCMI_lib.lib
