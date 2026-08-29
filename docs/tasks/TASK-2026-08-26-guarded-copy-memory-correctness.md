# Task Record: Guest Memory Access Correctness & Page-Crossing Recovery

**Task Date**: August 26-27, 2026  
**Status**: COMPLETE / VERIFIED  
**Owner Subsystem**: `Memory`, `HLE`, `Kernel`  

---

## 1. Problem Statement & Root Cause Discovery

During the execution of complex guest software (including PPSA21564), copies of guest structures spanning across page boundaries or touching demand-backed memory triggered fatal `0xC0000005` Access Violations in `VCRUNTIME140.dll` on guest worker stacks where host Structured Exception Handling (`__try/__except`) filters are bypassed by the Windows x64 unwinder.

Detailed architectural analysis revealed three fundamental design flaws in the memory subsystem:

1. **Coarse Region Commit State in `Memory::Query`**:
   When `Memory::Commit` or `Memory::Protect` was called on a sub-range (e.g. committing 64KB inside a 256MB reservation `r`), the region tracking flagged `r.committed = true` on the entire `Region` record. Consequently, subsequent calls to `Memory::Query(p)` for uncommitted pages inside the same reservation reported `is_committed = true` and valid read/write permissions, masking `MEM_RESERVE` pages from the emulator's memory guards.

2. **Fault-Prone `GuardedCopy` and Memory Helper Implementations**:
   `IsReadable` and `IsWritable` relied on `Query`, falsely validating uncommitted pages. `GuardedCopy` subsequently invoked `std::memmove` on uncommitted host pages, triggering hard kernel VEH crashes on guest worker threads.

3. **Concurrency Race Condition in Pool Allocator**:
   `PoolFree` manipulated pool bump pointers and free lists without acquiring `g_regions_mutex`, and `PoolFree` / `UntrackRegion` were separate operations, creating a race window where concurrent threads allocating and freeing pool blocks could corrupt region tracking.

---

## 2. Architecture & Implementation Changes

### 2.1 OS Ground-Truth Query Engine (`src/memory/memory.cpp`)
- `Memory::Query` now queries Windows `VirtualQuery` for non-pool allocations to obtain true OS page commit state (`mbi.State == MEM_COMMIT` vs `MEM_RESERVE` vs `MEM_FREE`), intersecting host page protection with guest region records.
- `IsReadable`, `IsWritable`, and `IsExecutable` walk 4KB host page slices (`kHostPageSize = 4096`), verifying every individual page in the requested range.

### 2.2 Unified Page-Aware Guarded Primitives (`src/memory/memory.h`, `src/memory/memory.cpp`)
- `Memory::GuardedRead(dest_host, src_guest, size, out_bytes_read)`: Chunk-by-chunk copy across 4KB pages into host buffer with transparent demand-commit for `MEM_RESERVE` pages and clean fault boundary tracking.
- `Memory::GuardedWrite(dest_guest, src_host, size, out_bytes_written)`: Chunk-by-chunk copy from host memory into guest memory.
- `Memory::GuardedCopy(dest_guest, src_guest, size, out_bytes_copied)`: Handles both forward and backward copies with full `memmove` overlap semantics across arbitrary 4KB page boundaries.
- `Memory::GuardedSet(dest_guest, value, size, out_bytes_set)`: Page-aware memory fill with transparent demand-commit.
- `Memory::GuardedStrlen`, `Memory::GuardedStrcpy`, `Memory::GuardedStrncpy`, `Memory::GuardedStrcmp`, `Memory::GuardedStrncmp`, `Memory::GuardedMemcmp`: Safe string and memory comparison primitives walking 4KB page slices without unbounded loops or SEH masking.

### 2.3 Subsystem Integration
- **`src/hle/libkernel.cpp`**: Replaced legacy `GuardedCopy`, `MemmoveImpl`, `MemcpyImpl`, `realloc`, `memset`, `strlen`, `strcpy`, `strncpy`, `strcat`, `strcmp`, `strncmp`, `strcasecmp` with `Memory::Guarded*`.
- **`src/hle/liblibc.cpp`**: Updated `MemcmpImpl`, `MemchrImpl`, `StrchrImpl`, `StrrchrImpl`, `StrcatImpl`, `StrncatImpl` to use `Memory::Guarded*`.
- **`src/kernel/syscalls.cpp`**: Updated `SafeReadBuffer` and `SafeWriteBuffer` to delegate to `Memory::GuardedRead` and `Memory::GuardedWrite`.

### 2.4 Thread-Safe Memory Pool Sub-Allocator
- `PoolAlloc` and `PoolFreeLocked` now execute under `g_regions_mutex`.
- `PoolAlloc` reuses free-list slots from `g_pool_free` with zero-initialization.
- `Memory::Unmap` atomically frees pool slots and erases region tracking under `g_regions_mutex`.

---

## 3. Verification & Evidence

1. **Unit Test Matrix (`tests/guest_memory_access_tests.cpp`)**:
   - Single-byte read/write/copy across start/middle/end of page: PASSED.
   - 2-page boundary crossing (spans across 4096 byte boundary): PASSED.
   - Multi-page crossing (64KB across 16 pages): PASSED.
   - Demand-commit across reserved pages: PASSED.
   - Fault boundary recovery on unmapped memory (halts cleanly without host SEH exception, partial byte tracking): PASSED.
   - Protection violation recovery: PASSED.
   - Overlapping copies (`memmove` forward and backward): PASSED.
   - String primitives across page boundaries: PASSED.
   - Multi-threaded concurrency stress test (1, 2, 4, 8, 16, 32 worker threads, 100 cycles each): PASSED with 0 errors.

2. **Full CTest Suite**:
   - 45/45 tests passed (100%).

3. **Title Execution Validation**:
   - **PPSA02929**: Sustained execution confirmed; draws frames via AGC graphics DCB submissions, presents swapchain frames, polls user services without memory regression.
   - **PPSA21564**: Successfully bootstraps, completes PRX linking, allocates 2GB direct memory pool, enters `GUEST_ENTRY_BEGIN` and runs without previous `GuardedCopy` `0xC0000005` crash.
