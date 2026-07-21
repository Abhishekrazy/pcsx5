@echo off
REM Builds the freestanding TLS test ELF used by the Phase-0 smoke test.
REM Requires clang on PATH (the LLVM toolchain bundled with Visual Studio works).
echo Compiling freestanding tls_guest.elf using Clang target x86_64-pc-linux-gnu...
clang -target x86_64-pc-linux-gnu -ffreestanding -nostdlib -fuse-ld=lld -Wl,--entry=_start -o tls_guest.elf tls_main.cpp
if %errorlevel% neq 0 (
    echo [ERROR] Failed to compile tls_guest.elf.
    echo Please make sure Clang is installed on your system (e.g., via Visual Studio Installer or LLVM website) and is in your PATH.
    exit /b %errorlevel%
)
echo [SUCCESS] Compiled tls_guest.elf successfully.
