@echo off
echo Compiling freestanding test_guest.elf using Clang target x86_64-pc-linux-gnu...
clang -target x86_64-pc-linux-gnu -ffreestanding -nostdlib -Wl,--entry=_start -o test_guest.elf main.cpp
if %errorlevel% neq 0 (
    echo [ERROR] Failed to compile test_guest.elf.
    echo Please make sure Clang is installed on your system (e.g., via Visual Studio Installer or LLVM website) and is in your PATH.
    exit /b %errorlevel%
)
echo [SUCCESS] Compiled test_guest.elf successfully.
