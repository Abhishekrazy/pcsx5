# TASK: P0 Boot Truth Recovery — Auxv / SceProcParam Consumption Recovery

**Date**: 2026-08-24  
**Status**: COMPLETE  
**Reference Audit**: `docs/audits/AUDIT-2026-08-24-auxv-sceprocparam.md`

---

## 1. Objectives
- Trace `libc` auxv consumption.
- Identify actual auxv comparison constants (instead of guessing `AT_SCE_LIBC_DATA`).
- Trace reads of `SceProcParam`.
- Determine whether `ProcParam` is accessed directly or through auxv.
- Trace allocator initialization and identify the exact failing state leading to `bad_alloc`.
- Implement ONLY after evidence is gathered.

## 2. Current State
- **Root Cause Identified**: The emulator's loader only executes `DT_INIT` for the main module. Dynamic libraries (like `libc.prx`) never have their `DT_INIT` executed.
- Because `libc` initialization is skipped, it never processes the auxiliary vector or `SceProcParam`.
- When `eboot.bin`'s `DT_INIT` runs and calls C++ static initializers that use the allocator, it crashes with `bad_alloc` due to the uninitialized `libc` state.
- **Next Task Required**: We must implement PRX initialization chaining. The loader must extract `DT_INIT` from all loaded PRX modules, and the guest initialization routine must invoke them in dependency order before the main module's `DT_INIT`.

## 3. Evidence Artifacts
- **Audit File**: `docs/audits/AUDIT-2026-08-24-auxv-sceprocparam.md`
- **Trap Logs**: Traces from `docs/evidence/2026-08/task-09-auxv/diag_trap12.txt` prove the `.init_array` loop was executing, but `Auxv` and `SceProcParam` were never read natively.
