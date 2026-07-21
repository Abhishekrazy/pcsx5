@echo off
REM Builds the full pcsx5 ELF test corpus in this directory.
REM
REM Categories produced (each = a "fixture" in the Phase-0 test corpus):
REM   test_guest.elf   freestanding syscall smoke test        ("valid", "file-I/O")
REM   tls_guest.elf    freestanding fs-relative TLS test     ("TLS")
REM   reloc_main.cpp   multi-section ELF with R_X86_64_RELATIVE  ("relocation")
REM   pie_main.cpp     position-independent ELF              ("PIE")
REM   fixed_main.cpp   fixed-address ELF                      ("fixed-address")
REM
REM Requires clang on PATH.  Run this script from any developer machine that
REM has LLVM/clang installed.  The resulting binaries are checked in alongside
REM the CTest build directory so the guest smoke tests can find them.
setlocal
set "HERE=%~dp0"
set "OUTDIR=%HERE%"

where clang >NUL 2>NUL
if errorlevel 1 (
    echo [WARN] clang is not on PATH.  Skipping corpus build.
    echo        Install LLVM/clang to produce the freestanding fixtures.
    exit /b 0
)

pushd "%HERE%"

echo [BUILD] test_guest.elf
clang -target x86_64-pc-linux-gnu -ffreestanding -nostdlib -fuse-ld=lld -Wl,--entry=_start -o "%OUTDIR%test_guest.elf"  main.cpp      || goto :error
echo [BUILD] tls_guest.elf
clang -target x86_64-pc-linux-gnu -ffreestanding -nostdlib -fuse-ld=lld -Wl,--entry=_start -o "%OUTDIR%tls_guest.elf"   tls_main.cpp  || goto :error
echo [BUILD] reloc_guest.elf
clang -target x86_64-pc-linux-gnu -ffreestanding -nostdlib -fuse-ld=lld -Wl,--entry=_start -o "%OUTDIR%reloc_guest.elf" reloc_main.cpp || goto :error
echo [BUILD] pie_guest.elf
clang -target x86_64-pc-linux-gnu -ffreestanding -nostdlib -fPIE -pie -Wl,--entry=_start -o "%OUTDIR%pie_guest.elf" pie_main.cpp  || goto :error
echo [BUILD] fixed_guest.elf
clang -target x86_64-pc-linux-gnu -ffreestanding -nostdlib -fuse-ld=lld -Wl,--entry=_start -o "%OUTDIR%fixed_guest.elf" fixed_main.cpp || goto :error

popd
echo [SUCCESS] ELF corpus built in %OUTDIR%.
endlocal
exit /b 0

:error
popd
echo [ERROR] Failed to build the ELF corpus.
endlocal
exit /b 1
