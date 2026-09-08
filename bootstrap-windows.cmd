@echo off
setlocal

set "PCSX5_PRESET=%~1"
if not defined PCSX5_PRESET set "PCSX5_PRESET=windows-x64-debug"
if not "%PCSX5_PRESET%"=="windows-x64-debug" if not "%PCSX5_PRESET%"=="windows-x64-release" if not "%PCSX5_PRESET%"=="windows-x64-graphics-debug" if not "%PCSX5_PRESET%"=="windows-x64-graphics-release" (
    echo Expected windows-x64-debug, windows-x64-release, windows-x64-graphics-debug or windows-x64-graphics-release.
    exit /b 1
)

set "PCSX5_VSROOT="
set "PCSX5_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%PCSX5_VSWHERE%" (
    echo Visual Studio Installer was not found.
    exit /b 1
)

for /f "usebackq delims=" %%I in (`"%PCSX5_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "PCSX5_VSROOT=%%I"
if not defined PCSX5_VSROOT (
    echo Visual Studio C++ build tools were not found.
    exit /b 1
)

call "%PCSX5_VSROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if not "%errorlevel%"=="0" exit /b %errorlevel%

pushd "%~dp0"
if not "%errorlevel%"=="0" exit /b %errorlevel%
cmake --preset %PCSX5_PRESET%
if not "%errorlevel%"=="0" goto failed

cmake --build --preset %PCSX5_PRESET%
if not "%errorlevel%"=="0" goto failed

ctest --preset %PCSX5_PRESET%
if not "%errorlevel%"=="0" goto failed
popd
exit /b 0
:failed
popd
exit /b 1
