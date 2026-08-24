# AUDIT-2026-08-24-libc-startup.md

## 1. Objective
Investigate the runtime environment and initialization sequence of `libc.prx` on PS5 as observed during the execution of PPSA21564 and PPSA02929. The goal is to determine why the execution halts at the UNKNOWN stub `7H0iTOciTLo#A#B`, establish the calling contract, and determine the structural defects in the current PRX initialization model.

## 2. libc Initialization Call Graph
The actual call graph during PPSA21564 boot was:
1. `Kernel::Execute` jumps to `eboot.bin` `_start` (`0x800000070`).
2. `eboot.bin` `_start` calls `bzQExy189ZI#T#T` (`_init_env`) at RIP `0x800000089`, passing `rsp` (the guest stack pointer).
3. The call to `_init_env` is intercepted by the PCSX5 HLE stub `libkernel::bzQExy189ZI#T#T`, which incorrectly returns `0` and ignores the environment initialization for `libc.prx`.
4. `eboot.bin` `_start` calls `8G2LB+A3rzg#T#T` (`atexit`) twice, which are also intercepted by HLE stubs.
5. `eboot.bin` `_start` calls `DT_INIT` (`0x800000010`), which iterates over `DT_INIT_ARRAY`.
6. A global constructor in `eboot.bin` invokes a function in `libc.prx` (at `0x820033cd7`).
7. `libc.prx`, being uninitialized, calls `7H0iTOciTLo#A#B` (`pthread_mutex_lock`), which is an UNKNOWN stub and terminates the process.

## 3. DT_INIT Findings
The assumption in Task 10 that PRX initialization was successfully executing is **FALSE**.
- `XKRegsFpEpk` (`sceLibcInitialize`) is **never called** by the statically-linked `_start` routine of PPSA21564 and PPSA02929.
- Because `XKRegsFpEpk` is bypassed, the PRX initialization queue `g_prx_init_queue` built by the loader is **never processed**.
- Therefore, PRX modules (including `libc.prx` and `libkernel.prx`) are running with uninitialized global state.

## 4. DT_INIT_ARRAY Findings
- `eboot.bin` executes its own `DT_INIT_ARRAY` manually via its `DT_INIT` function (called directly from `_start`).
- `libc.prx`'s `DT_INIT_ARRAY` is never executed because the PRX init queue is never flushed by the kernel/loader.

## 5. Process Environment Findings
- `eboot.bin` `_start` passes the raw stack pointer `rsp` to `_init_env`.
- The stack contains `argc`, `argv`, `envp`, and the Auxiliary Vector (`auxv`).
- Because `_init_env` is stubbed out by HLE, `libc.prx` never parses this stack, meaning `libc.prx` is entirely unaware of the process parameters.

## 6. Auxv / SceProcParam Findings
- `SceProcParam` and `auxv` reads were **NOT** observed within `libc.prx` because `_init_env` was intercepted by the emulator.
- `_init_env` expects to read `auxv` from the provided stack pointer to extract the real PS5 environment (such as `AT_PHDR`, `AT_ENTRY`, and memory sizes).

## 7. UNKNOWN Stub Analysis
- **NID:** `7H0iTOciTLo#A#B`
- **Friendly Name:** `pthread_mutex_lock` (from `libkernel.prx`)
- **Classification:** Synchronization Dependency
- **Context:** Called by `libc.prx` (at `0x820033cd7`) during an early allocation or lock acquisition attempt (triggered by an `eboot.bin` global constructor), which fails because `libc` is not initialized.

## 8. Calling Contract for 7H0iTOciTLo#A#B (pthread_mutex_lock)
- `RDI`: `0x820198a40` (pointer to a `pthread_mutex_t` structure in `libc.prx`'s .bss or .data).
- `RSI`: `0x90`
- `RDX`: `0x90`
- **Classification:** `NEVER_SAFE`. A fake successful return (returning 0) would allow multi-threaded code to bypass synchronization, leading to internal memory corruption in guest data structures.

## 9. Conclusion & Claims vs Reality
- **Claim from Task 10:** PRX initialization lifecycle is recovered.
- **Reality:** PRX modules are NOT being initialized. The guest entry point (`_start`) in retail ELFs expects PRX dependencies to already be initialized by the loader, or it expects `_init_env` to do it. Currently, the emulator intercepts `_init_env` and does not run PRX initializers prior to jumping to the entry point.
- **Next Boundary:** We must remove the HLE stub for `_init_env` (`bzQExy189ZI#T#T`) and ensure that the real `libc.prx` function executes and parses the guest stack. If `_init_env` is responsible for initializing PRXs, tracing its execution will reveal the exact process environment expected. Alternatively, the emulator's `Kernel::Execute` must run the PRX initialization queue before jumping to `eboot.bin`'s entry point.
