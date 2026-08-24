# TASK REPORT: libc.prx DT_INIT Environment Contract Recovery

## Status
INVESTIGATION COMPLETE

## Objective
Dissect the origin of the `0xa0020013` fatal library initialization exception during `libc.prx` `DT_INIT` execution, and determine the exact environmental contracts that the host kernel must provide to the PRX initialization queue.

## Key Findings

### 1. The `0xa0020013` Exception is a Symptom, Not a Trap
The exception `0xa0020013` is an explicit, deliberate failure code raised by `libc.prx` itself when it detects that the host/kernel execution environment is incomplete.

The call chain leading to the crash is:
1. `libc.prx` -> `DT_INIT` (starts execution).
2. The initialization function calls an internal validation subroutine (located at `0x60d0` in the PRX image).
3. The subroutine explicitly tests a return condition. If it fails, it issues `mov edi, 0xa0020013` and calls `sceKernelDebugRaiseException` via the PLT.

### 2. The Failed Contract: `KernelGetProcParam`
The internal validation subroutine at `0x60d0` relies on the kernel to provide the process parameter block:
1. It calls the HLE thunk for `KernelGetProcParam` (NID `959qrazPIrg`).
2. Prior to this investigation, our HLE implementation for `KernelGetProcParam` trivially returned `0`.
3. The `libc.prx` validation subroutine tests the returned pointer (the `SceProcessParam` block) for `NULL`. When it encounters `NULL` (or an invalid size), it aborts initialization and throws `0xa0020013`.

### 3. The `SceProcessParam` Contract
The validation subroutine enforces a strict schema on the process parameter block returned by `KernelGetProcParam`:
- It expects the return value `[rax]` to contain the base `SceProcessParam` struct.
- It validates that the size (`[rax]`) is `>= 0x40`.
- It expects an extension struct pointer at `[rax + 0x38]`.
- It validates the extension struct:
  - Extension size (`[ext]`) must be `>= 0x40` (and later `>= 0x68`).
  - Version (`[ext + 0x08]`) must be `>= 2`.
  - Type (`[ext + 0x0C]`) must be `== 1`.

### 4. Proof of Correctness
By mapping `KernelGetProcParam` to return a dummy process parameter struct that precisely satisfies these layout checks, the validation subroutine at `0x60d0` returned `0` (SUCCESS). The `test eax, eax` check passed, jumping entirely over the `sceKernelDebugRaiseException` block and continuing normal PRX initialization.

This proves that:
1. **The Architecture is Correct:** HLE-driven `DT_INIT` before `_start` is the correct execution order. HLE just failed to construct the right environment metadata.
2. **No Hack is Needed:** The emulator does not need to suppress the exception, skip initializers, or fabricate a thread context hack. We simply need to accurately parse and return the real `PT_SCE_PROC_PARAM` segment provided by the PS5 executable.

## Next Steps
The emulator loader currently ignores the `PT_SCE_PROC_PARAM` segment (0x6FFFFF00) during loading.

The next engineering phase should implement proper loader support for parsing `PT_SCE_PROC_PARAM` from the main `ElfModule` (`eboot.bin`), surfacing it via `Kernel::GetMainModuleProcessParam()`, and feeding it to `KernelGetProcParam` to structurally satisfy all PRX initializers natively.

*Note: As `libc.prx` continues initialization, it hits a subsequent exception (`0xC0000005`) at `0x82000aaff`. This indicates another missing environmental dependency (likely related to TLS binding or memory access), which will be investigated in the next task.*

