# AUDIT: libc.prx Runtime Foundation

## 1. Module Ownership
- **Owner**: Kernel (for initialization trigger), `libc.prx` (for internal state).
- **Initialization Lifecycle**: [VERIFIED] PRX `DT_INIT` -> PRX `DT_INIT_ARRAY` -> `eboot` `_start` -> `libc` `_init_env` -> guest C/C++ runtime.

## 2. Global/Static State
- `libc.prx` maintains its own internal allocator, thread-local storage, and process metadata derived from `PT_SCE_PROC_PARAM`.

## 3. Process Environment Dependencies
- **SceProcParam**: [VERIFIED] `PT_SCE_PROC_PARAM` (0x61000001). Passed to `libc.prx` via HLE `KernelGetProcParam` (`959qrazPIrg`). If missing, `libc.prx` raises `0xa0020013`.

## 4. Kernel Dependencies
- [VERIFIED] `KernelGetProcParam`: Must return the address of the main module's `PT_SCE_PROC_PARAM` segment.
- [VERIFIED] `sceKernelDebugRaiseException`: Called internally by `libc` on fatal environment contract failures.

## 5. TLS Dependencies
- [OBSERVED] Handled natively by `libc.prx` using the `SceProcParam` structures and standard Thread Control Block layout once the kernel provides the correct metadata.

## 6. Allocator Dependencies
- [VERIFIED] The allocator is initialized natively inside `libc.prx` during `DT_INIT` before `eboot` `_start` executes.

## 7. Current Known Failures
- **None in libc.prx**. Execution successfully transitions to the game executable. The next crash boundary is `sceKernelMapNamedDirectMemory` invoked by the game engine.

## 8. Evidence Classification
- **KernelGetProcParam requirement**: [VERIFIED]
- **PT_SCE_PROC_PARAM type 0x61000001**: [VERIFIED]
- **libc.prx initialization ordering**: [VERIFIED]
