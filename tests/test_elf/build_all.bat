@echo off
REM Builds every freestanding test ELF in this directory.  Each ELF exercises a
REM specific guest-execution path: syscall bridging, TLS access, and (later)
REM file I/O, threads, etc.  The Phase-0 CTest target invokes this script
REM before running the emulator against the produced binaries.
setlocal

set "ELFS=test_guest.elf tls_guest.elf"

if "%~1"=="" (
    set "OUTDIR=%~dp0"
) else (
    set "OUTDIR=%~1\"
)

pushd "%~dp0"

for %%F in (%ELFS%) do (
    if exist "%%~nF_main.cpp" (
        echo [BUILD] %%F
        clang -target x86_64-pc-linux-gnu -ffreestanding -nostdlib -fuse-ld=lld -Wl,--entry=_start -o "%OUTDIR%%%F" "%%~nF_main.cpp"
        if errorlevel 1 (
            echo [ERROR] Failed to compile %%F.
            popd
            exit /b 1
        )
    ) else (
        echo [SKIP] %%~nF_main.cpp not found.
    )
)

popd
echo [SUCCESS] All test ELFs built.
endlocal
