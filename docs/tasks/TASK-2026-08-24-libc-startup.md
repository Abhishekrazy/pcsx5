# TASK-2026-08-24-libc-startup.md

## Objective
**EXECUTE PRX INITIALIZATION QUEUE IN KERNEL LOADER**

## Background
In Task 10, the PRX initialization queue was hooked into the HLE stub for `XKRegsFpEpk` (`sceLibcInitialize`). However, runtime evidence from PPSA21564 and PPSA02929 demonstrates that retail game ELFs do **NOT** invoke `XKRegsFpEpk`. Instead, their `_start` routine manually calls `_init_env` and their own `DT_INIT`, which subsequently calls into uninitialized `libc.prx` routines.

This confirms that the PS5 loader (kernel) is architecturally responsible for executing the PRX `DT_INIT` queue *before* transferring execution to the main game ELF's `e_entry`.

## Implementation Requirements
1. **Remove PRX Init from `XKRegsFpEpk`**: Remove the PRX initialization loop from `XKRegsFpEpk` in `src/hle/libkernel.cpp`, as it does not accurately model the PS5 boot sequence for retail ELFs.
2. **Execute PRX Init in Loader**: Modify `Kernel::Execute` in `src/kernel/kernel.cpp` to process `g_prx_init_queue` immediately prior to calling `StartGuestCaptured` (or jumping to `e_entry`). 
3. **Instrumentation**: Ensure that the PRX execution logs (`MODULE_INIT_BEGIN` / `MODULE_INIT_END`) clearly show the `DT_INIT` routines of all dependencies (like `libc.prx` and `libkernel.prx`) running before `eboot.bin`'s entry point is reached.
4. **Environment Check**: Ensure that PRX initializers receive a valid execution environment (i.e. if they execute on the main thread, the stack pointer `sp` and `argv`/`auxv` setup must be preserved and valid before calling them).

## Success Criteria
- Running PPSA21564 must output `MODULE_INIT_BEGIN` for `libc.prx` and other dependencies *before* printing `Starting execution of u12wg.0 at Entry Point: 0x800000070`.
- Tests must pass (46/46).
- The boot log must accurately reflect the dependency initialization chain, moving the next failure boundary from the uninitialized `eboot.bin` constructor into the actual `libc.prx` initialization code.
