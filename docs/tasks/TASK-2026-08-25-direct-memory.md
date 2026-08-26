# TASK REPORT: Direct Memory Contract Recovery

## 1. MODULE EXECUTIVE SUMMARY
The guest Direct Memory allocation contract has been recovered and verified. By observing the native transition of PPSA21564 from `libc.prx` initialization into the game engine, we documented the exact calling convention and semantics of `sceKernelAllocateDirectMemory` and `sceKernelMapNamedDirectMemory`. The implementation integrates natively with the emulator's `Memory::` physical pool model.

## 2. PART 1: VERIFICATION OF PREVIOUS COMPLETION
The previous task's claim of `libc.prx` self-sufficiency is **[VERIFIED]**.
- `PT_SCE_PROC_PARAM` is correctly defined as `0x61000001` and `PT_SCE_PATH` as `0x6FFFFF00`.
- The loader natively extracts this segment and `KernelGetProcParam` returns the true offset.
- `libc.prx` correctly bootstraps TLS and its allocator using this structure without crashing.
- No speculative fallback was necessary.

## 3. PART 2 & 3: DIRECT MEMORY CALL CHAIN
**Allocation Phase:**
`sceKernelAllocateDirectMemory(len=0x79800000, align=0x200000)`
- The emulator reserves space in the physical pool and returns the **physical offset** (`0x200000`).

**Mapping Phase:**
`sceKernelMapNamedDirectMemory(name="orbis_user_malloc", len=0x79800000, prot=0xF3, physOff=0x200000, align=0x200000)`
- The guest explicitly passes the offset returned by the allocator.
- The emulator commits host memory at `pool_base + physOff`.
- It returns the mapped **virtual address** back to the guest.

## 4. PART 5: MEMORY MODEL INTEGRATION
The existing `g_phys_pool_base` reservation system seamlessly represented the Direct Memory abstraction without creating a parallel allocator. The lifecycle is guaranteed by `Hle::Shutdown() -> ResetPhysPool()`, which completely frees the virtual reservation.

## 5. TEST RESULTS (PPSA21564 / PPSA02929)
- **Previous state**: Fatal exception inside `libc.prx`.
- **Current state**: Execution cleanly passes PRX init, enters `eboot.bin` `_start`, initializes the C++ runtime, and successfully allocates a 1.9 GB direct memory arena (`orbis_user_malloc`) using the newly implemented `sceKernelMapNamedDirectMemory`.
- **Next boundary**: Halts at `sceKernelIsAddressSanitizerEnabled`.

## 6. CLAIMS VS REALITY
| Claim | Evidence | Status |
|-------|----------|--------|
| AllocateDirectMemory returns a handle/VA | Guest Register Trace | **[FALSIFIED]** It strictly returns a 0-based physical offset. |
| MapNamedDirectMemory has a different signature | Crash Log Registers | **[FALSIFIED]** It matches `sceKernelMapDirectMemory` exactly, with a name pointer appended on the stack (`[RSP+8]`). |

## 7. EXACTLY ONE NEXT TASK
Implement the `sceKernelIsAddressSanitizerEnabled` stub and verify the subsequent game engine startup sequence.
