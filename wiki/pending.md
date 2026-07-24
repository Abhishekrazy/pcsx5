# PCSX5 Boot Blocker Priorities (KytyPS5 Comparison)

> Kyty source cloned from https://github.com/InoriRus/Kyty
> Last updated: 2026-07-24

## PRIORITY 1 — Fixed This Sprint
- ✅ **XKRegsFpEpk** — Now calls main(argc, argv, envp) instead of ExitGuestProcess(argc)
- ✅ **Atexit stub (rsi)** — _start now receives valid atexit callback in rsi (was NULL)
- ✅ **GPU headless mode** — Boots without GLFW window via PCSX5_HEADLESS=1
- ✅ **Thread isolation** — Guest crash no longer kills the emulator process
- ✅ **Build script** — VS auto-detect, FFmpeg DLL staging, organized dist

## PRIORITY 2 — HLE Gaps vs Kyty (Need to Port)

### Missing Thread Atexit Tracking (Kyty: Pthread.cpp lines 380-414)
Kyty has `KernelSetThreadAtexitCount`, `KernelSetThreadAtexitReport`, `KernelRtldThreadAtexitIncrement/Decrement`. These track the number of atexit handlers per thread. We don't have these — if the game's libc calls them during thread creation/exit, our stubs return 0 which may cause incorrect cleanup.

**Action**: Port from `/tmp/kyty-check/source/emulator/src/Libs/LibKernel.cpp:382-414`

### Missing Thread-Specific Data TLS Initialization  
Kyty initializes TLS with a specific layout matching PS5's libc expectations. Our `Kernel::Initialize` sets up a basic TLS block (self-pointer at tp[0], canary at tp[40]) but it may not match PS5's exact layout. The Kyty approach uses `RuntimeLinker::TlsGetAddr` and proper thread-local image initialization.

**Action**: Compare `RuntimeLinker.cpp:503-550` TLS init with our `kernel.cpp:358-398`

### Missing Full PRX Module Loading from System Memory
Kyty's `RuntimeLinker` handles `sceKernelLoadStartModule` by loading real PRX files from firmware directories. Our implementation at `libkernel.cpp:743` tries this but falls back to HLE. Kyty ships stub PRX files that provide proper export tables — this makes the game's loader happy even when the actual implementation is HLE.

**Action**: Either ship stub PRX files or ensure our HLE covers every NID the PRX would export

### Missing sceKernelGetProcessType with Correct Semantics
We return 1 for main process. Kyty likely returns the correct Orbis value. Check if games check specific bits.

### Missing Proper Module Init/Fini Callbacks
Kyty's `RuntimeLinker::Start()` calls DT_INIT and DT_FINI through the PS5's `module_ini_fini_func_t` protocol. Our `XKRegsFpEpk` handler now calls `InvokeGuestFunction(dt_init, 0, 0, 0)` but the PS5 protocol expects specific arguments (args count + argp + func pointer).

**Action**: Check Kyty's `run_ini_fini()` at `RuntimeLinker.cpp:92-95`

## PRIORITY 3 — Performance / Correctness

- **Thread creation rate** — Many games create 30+ threads at boot. Our `scePthreadCreate` handler allocates TLS/stack for each thread via VirtualAlloc. This is slow but correct.
- **Synchronization performance** — Our mutex/condvar/event flag implementations use real Windows sync primitives. Performance should be good.
- **File I/O path translation** — Guest paths like /app0/... need proper resolution. Our `TranslateGuestPath` handles this.

## How to Debug Next Boot Attempt

Run with:
```powershell
$env:PCSX5_HEADLESS=1
.\build\bin\Release\pcsx5_cli.exe --boot "D:\Games\YourGame\eboot.bin" --log-level=debug 2> boot.log
```

Check `boot.log` for:
1. "Entry point code" — shows first 32 bytes at the guest entry point
2. "LogStubCallOnce" — shows which HLE functions are being called as stubs
3. "Unimplemented stub called" — NIDs we don't handle
4. Heartbeat counters — TLS traps, patches, elapsed time
