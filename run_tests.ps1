# Runs OmniAI's unit tests (build\omniai_tests.exe, made by build.bat).
# VCMI_lib.dll is read from $env:VCMI_BIN, a VCMI install folder (the same variable
# CMakeLists.txt reads); tbb12.dll from ..\resources\onetbb. Nothing is written there.
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $env:VCMI_BIN) {
	Write-Error "Set VCMI_BIN to a VCMI install folder (the one holding VCMI_lib.dll)."
	exit 2
}
$tbb = Join-Path $here '..\resources\onetbb\oneapi-tbb-2022.2.0\redist\intel64\vc14'
$env:PATH = "$env:VCMI_BIN;$tbb;" + $env:PATH
& (Join-Path $here 'build\omniai_tests.exe')
exit $LASTEXITCODE
