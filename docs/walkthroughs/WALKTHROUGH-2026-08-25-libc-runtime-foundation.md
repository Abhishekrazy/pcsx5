# Walkthrough: libc.prx DT_INIT Environment Contract Recovery

## Investigation Summary
We successfully discovered the origin of the `0xa0020013` exception that was causing `libc.prx` to fail during its initialization sequence (`DT_INIT`).

1. **Identifying the Source**: We traced the exception back to an internal subroutine at `0x8200060d0` inside `libc.prx`. The exception was not a crash in the emulator's execution flow itself, but an explicitly thrown fatal error by the library when a validation check failed.
2. **Identifying the Dependency**: We dumped the assembly of `0x8200060d0` and found that it calls `KernelGetProcParam` (`959qrazPIrg`), expecting a valid pointer to a `SceProcessParam` structure.
3. **Analyzing the Validation Logic**: By tracing the assembly checks, we discovered the precise validation constraints `libc.prx` enforces on `SceProcessParam`:
   - `[rax]` (size) must be `>= 0x40`.
   - `[rax + 0x38]` must contain a pointer to an extensions block.
   - The extensions block must have its own size `>= 0x68`, version `>= 2`, and type `== 1`.
4. **Verifying the Hypothesis**: We modified our HLE stub for `KernelGetProcParam` to return a dynamically allocated, dummy struct satisfying these exact layout checks. 
5. **Success**: When running the executable with the dummy struct, the validation logic in `0x60d0` returned `0` (Success), safely bypassing the `0xa0020013` exception and continuing initialization.

## Conclusion
This definitively proves that the **HLE-driven `DT_INIT` before `_start` architecture is correct**. The crash was entirely a result of missing boot environment metadata (the `SceProcessParam`), which we must now implement correctly by parsing the `PT_SCE_PROC_PARAM` segment from the PS5 executable (`eboot.bin`).

*(Note: The emulator subsequently crashed at `0x82000aaff` with `0xC0000005`, indicating the next missing dependency in the PRX initialization chain, which we will address in the next task).*
