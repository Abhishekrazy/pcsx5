# AUDIT: Direct Memory Contract

## 1. Module Ownership
- **Owner**: `libkernel` (Memory Subsystem).
- **Initialization Lifecycle**: Direct memory is available after Kernel initialization and is used by the guest game engine (e.g., `orbis_user_malloc` allocator).

## 2. API Contract: `sceKernelAllocateDirectMemory`
- **Arguments**: `search_start` (RDI), `search_end` (RSI), `length` (RDX), `alignment` (RCX), `mem_type` (R8), `out_ptr` (R9).
- **Semantics**: Allocates contiguous physical memory from the system pool.
- **Return Value**: Returns `0` on success. Writes a **physical offset** (not a handle, not a virtual address) into the memory pointed to by `out_ptr`.
- **Evidence Classification**: [VERIFIED] PPSA21564 requested `len=0x79800000`, the implementation returned `offset=0x200000`. The game immediately passed `0x200000` to `sceKernelMapNamedDirectMemory` as the physical offset.

## 3. API Contract: `sceKernelMapNamedDirectMemory`
- **Arguments**: `addr_out` (RDI), `len` (RSI), `prot` (RDX), `flags` (RCX), `phys_offset` (R8), `alignment` (R9), `name_ptr` (RSP+8).
- **Semantics**: Maps previously allocated direct memory (represented by `phys_offset`) into the guest's virtual address space.
- **Return Value**: Returns `0` on success. Writes the mapped **guest virtual address** into `addr_out`.
- **Evidence Classification**: [VERIFIED] PPSA21564 successfully used the output of this mapping and continued execution until encountering the next architectural stub.

## 4. Virtual Address Ownership and Alignment
- **Guest vs Host**: The returned Virtual Address is inherently host-backed and mapped identity to the guest.
- **Alignment**: The alignment requested by the guest is strictly preserved by padding the `phys_offset` and committing at the aligned boundary.

## 5. Lifecycle and Reset Behavior
- **Commit**: Memory is committed to the host OS strictly at mapping time (`MapDirectMemoryCore`), not at allocation time, accurately representing reservation semantics.
- **Shutdown**: The entire direct memory pool (`g_phys_pool_base`) is unmapped and released back to the OS via `VirtualFree(..., MEM_RELEASE)` during `Hle::Shutdown() -> ResetPhysPool()`.
- **Evidence Classification**: [VERIFIED] No memory leaks or stale handles survive the reset boundary.

## 6. Current Known Failures
- Execution now clears Direct Memory initialization and halts at `sceKernelIsAddressSanitizerEnabled`.

## 7. Supporting Operations
- `sceKernelReleaseDirectMemory` and `sceKernelMunmap` remain unimplemented, as the current guest trace has not yet invoked them. They will be implemented strictly when requested by evidence.
