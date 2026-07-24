# PCSX5 Status & Pending Work

> Last updated: 2026-07-25

## ✅ Working

### Core Emulation
- ✅ Subsystem init (Config, Memory, HLE, Kernel, GPU)
- ✅ ELF/SELF loading, module linking, PRX resolution
- ✅ TLS patching (fs: segment access emulation)
- ✅ Thread creation (scePthreadCreate via Windows threads)
- ✅ Synchronization (mutex, condvar, event flags, semaphores)
- ✅ VideoOut (flip/queue model, VRR, vblank pump)
- ✅ AGC/GnmDriver (M3 draw execution, shader translation)
- ✅ AudioOut (WASAPI/XAudio2/SDL backends)
- ✅ File I/O (guest path translation, open/read/write/stat)

### Boot Sequence
- ✅ XKRegsFpEpk → calls main(argc, argv, envp)
- ✅ Atexit stub in rsi (was NULL, causes crash)
- ✅ DT_INIT runs before main (global constructors)
- ✅ GPU headless mode (PCSX5_HEADLESS=1)
- ✅ Thread isolation (guest crash doesn't kill emulator)

### HLE Coverage
- ✅ Thread atexit tracking (ported from Kyty)
- ✅ ~20 boot-critical HLE stubs (KernelGetProcParam, tls_get_addr, etc.)
- ✅ SEH-guarded strcmp/strncmp/strcasecmp

### WPF Frontend
- ✅ IPC mode (out-of-process core via shared memory + pipe)
- ✅ Console logs with line numbers, colors, dedup, auto-scroll
- ✅ Boot overlay / pause menu (visible inside emulator area only)
- ✅ Kill/Stop/Cancel buttons work (child process termination)
- ✅ Session restart after crash/stop
- ✅ SetDllDirectory for plugins/ folder

### Build System
- ✅ build_release.ps1 stages dist/ with plugins/ + tools/ layout
- ✅ FFmpeg DLL staging from SharpEmu or auto-download
- ✅ VS version auto-detection

## 🔴 Current Issues

### 1. Game Crashes with 0xC0000005 in VCRUNTIME140.dll
- Happens during strcmp calls with bad guest pointers
- SEH handler catches AV and returns -1, but process still dies
- **Fix attempted**: 4MB stack, SEH guard — still crashing
- **Root cause**: Unknown — might need to investigate further

### 2. IPC Frame Display
- ✅ RenderFrame now writes to IPC in headless mode (was skipping due to g_window check)
- 🟡 Need to verify frames appear in WPF UI

### 3. Boot overlay hides console in left/bottom dock modes
- ✅ Fixed with Panel.ZIndex and RowSpan=1

## 🟡 Medium Priority (Next Sprint)

### HLE Gaps vs Kyty
- TLS initialization comparison with Kyty's RuntimeLinker
- PRX stub files / full HLE coverage for remaining NIDs
- Module init/fini callback protocol (DT_INIT args)

### Performance
- Thread creation rate (30+ threads at boot)
- File I/O path caching

## Build & Test
```powershell
.\build_release.ps1
$env:PCSX5_HEADLESS=1
.\dist\pcsx5_cli.exe --title-id=PPSA02929 "Games\PPSA02929-app01\eboot.bin"
```

Check `boot.log` for:
1. "Entry point code" — shows first 32 bytes at the guest entry point
2. "LogStubCallOnce" — shows which HLE functions are being called as stubs
3. "Unimplemented stub called" — NIDs we don't handle
4. Heartbeat counters — TLS traps, patches, elapsed time
