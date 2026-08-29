# AUDIT: P0 BOOT TRUTH: AUXV / SCE PROC PARAM CONSUMPTION RECOVERY

**Date**: 2026-08-24
**Task**: TASK 09

## 1. Background
PPSA21564 (`Brotato`) exhibits a `std::bad_alloc` during static initialization (`DT_INIT`). The game's C++ allocator is statically linked, suggesting the failure arises from `libc.prx` initialization (which the allocator depends on).
Previous experiments injected standard Linux/FreeBSD auxiliary vector entries (`AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_ENTRY`), but the `bad_alloc` persisted.

Current verified evidence:
- `PT_SCE_PROC_PARAM` = `0x61000001`
- `SceProcParam` size = `0x60`
- `SceProcParam` magic = `ORBI`

Missing evidence:
- The exact `AT_SCE_LIBC_DATA` tag expected by `libc.prx`.
- The exact meaning of the `0x411` field in `SceProcParam` (suspected to be related to heap sizes).
- The actual access pattern `libc.prx` performs during startup.

## 2. Methodology
To strictly adhere to `GEMINI.md` (no speculative PS5 behavior, evidence before implementation), we will use hardware-level trapping (Vectored Exception Handling) during the guest boot process to trace:
1. Reads to the Auxiliary Vector array.
2. Reads to the `SceProcParam` memory block.

When the guest accesses these protected memory regions, we will intercept the fault, dump the RIP and registers (such as RAX/RCX/RDX, which will hold the comparison tags and offsets), and resume execution to map out the exact consumption pattern before the `bad_alloc` occurs.

## 3. Investigation Log
Analysis of `docs/evidence/2026-08/task-09-auxv/diag_trap12.txt` from a previous run reveals the following sequence:
1. `SceProcParam` trap (PAGE_GUARD) triggered at `RIP=0x800000054`.
2. The read was at offset `-0x20` from `SceProcParam`, executed by a backwards scanning loop:
   ```asm
   48 83 C3 F8    add rbx, -8
   48 8B 03       mov rax, [rbx]
   48 85 C0       test rax, rax
   ...
   FF D0          call rax
   ```
3. This is the `.init_array` static initializer loop for the *main* module (`eboot.bin`), which is physically adjacent to `SceProcParam` in memory.
4. No traps were triggered for `Auxv` or `SceProcParam` directly.
5. The `bad_alloc` occurred *inside* one of `eboot.bin`'s static initializers (`0x8074d2670`).

**Root Cause Discovery**: The emulator's loader (`Loader::LinkLoadedPrxModules` and `Kernel::Execute`) only extracts and executes `DT_INIT` for the *main* module (`eboot.bin`). It completely ignores `DT_INIT` for all dynamically loaded PRXs (like `libc.prx`). Because `libc.prx` is never initialized, it never parses the Auxiliary Vector, never consumes `SceProcParam`, and the C++ allocator subsequently fails when `eboot.bin` attempts to use it.

## 4. Claims vs Reality
- **Claim**: `PPSA21564` allocator depends on a specific, unknown `AT_SCE_LIBC_DATA` tag.
- **Reality**: `libc` initialization is entirely skipped by the PCSX5 loader. The auxiliary vector is never read because the code responsible for reading it (`libc.prx` `DT_INIT`) is never executed.
- **Action Required**: The guest loader must be updated to extract `DT_INIT` for all loaded PRX modules, and the guest initialization chain (e.g., `xk_regs_fpepk`) must execute these in reverse topological order (dependencies first) before executing the main module's `DT_INIT`.
