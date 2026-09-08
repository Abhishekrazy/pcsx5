@echo off
setlocal
set "PCSX5_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "PCSX5_VSROOT="
if not exist "%PCSX5_VSWHERE%" exit /b 1
for /f "usebackq delims=" %%I in (`"%PCSX5_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "PCSX5_VSROOT=%%I"
if not defined PCSX5_VSROOT exit /b 1
call "%PCSX5_VSROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if not "%errorlevel%"=="0" exit /b %errorlevel%
pushd "%~dp0.."
if not "%errorlevel%"=="0" exit /b %errorlevel%
cmake -S tests/characterization -B out/build/characterization-windows-x64 -G Ninja -DCMAKE_BUILD_TYPE=Debug
if not "%errorlevel%"=="0" goto failed
cmake --build out/build/characterization-windows-x64
if not "%errorlevel%"=="0" goto failed
ctest --test-dir out/build/characterization-windows-x64 --output-on-failure --no-tests=error
if not "%errorlevel%"=="0" goto failed
popd
exit /b 0
:failed
popd
exit /b 1
