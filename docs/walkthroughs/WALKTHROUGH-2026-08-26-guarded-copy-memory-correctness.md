# Walkthrough: Guest Memory Access Correctness & Page-Crossing Recovery

## Overview

This walkthrough documents the end-to-end changes made to the PCSX5 memory subsystem to ensure safe, page-aware guest memory operations, transparent demand-committing of reserved memory, clean fault boundary recovery on unmapped addresses without host SEH crashes, and thread-safe pool allocation.

---

## Key Changes

### 1. Memory Subsystem Primitives (`src/memory/memory.h`, `src/memory/memory.cpp`)
- Added declarations and implementations for `GuardedRead`, `GuardedWrite`, `GuardedCopy`, `GuardedSet`, `GuardedStrlen`, `GuardedStrcpy`, `GuardedStrncpy`, `GuardedStrcmp`, `GuardedStrncmp`, and `GuardedMemcmp`.
- Updated `Memory::Query` to query `VirtualQuery` for OS ground-truth state on non-pool allocations.
- Updated `IsReadable`, `IsWritable`, and `IsExecutable` to walk 4KB host pages across the requested range.
- Made `PoolAlloc` and `PoolFreeLocked` thread-safe with free-list recycling under `g_regions_mutex`.

### 2. HLE and Kernel Refactoring
- Refactored `src/hle/libkernel.cpp` to replace local, brittle copy/string functions with `Memory::Guarded*`.
- Refactored `src/hle/liblibc.cpp` to use `Memory::GuardedMemcmp`, `Memory::GuardedStrlen`, `Memory::GuardedStrcpy`, and safe page-validated `memchr`/`strchr`.
- Refactored `src/kernel/syscalls.cpp` (`SafeReadBuffer`, `SafeWriteBuffer`) to use `Memory::GuardedRead` and `Memory::GuardedWrite`.

### 3. Verification Suite (`tests/guest_memory_access_tests.cpp`)
- Implemented comprehensive test coverage for single-byte accesses, 2-page crossings, 64KB multi-page copies, demand-commit on `MEM_RESERVE`, fault boundary recovery on `MEM_FREE`, read-only protection enforcement, forward/backward overlapping copies, string primitives across page boundaries, and 1..32 thread concurrency stress tests.
- Registered target `guest_memory_access_tests` in `CMakeLists.txt`.

---

## Validation Results

- **Unit Tests**:
  - `guest_memory_access_tests.exe`: 79/79 checks PASSED.
  - `memory_query_tests.exe`: 123/123 checks PASSED.
- **Full CTest Suite**:
  - 45/45 tests PASSED (100%).
- **Interactive Title Runs**:
  - PPSA02929 sustained execution verified with active frame drawing and swapchain present.
  - PPSA21564 executes through boot sequence, PRX init, and guest entry point without memory access violations.
