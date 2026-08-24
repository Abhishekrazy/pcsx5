# AUDIT: HLE Direct Memory Physical Pool Ownership & Teardown

- Date: 2026-08-23
- Baseline Commit: 9b2d8ff (main)
- Build: Release x64 (MSVC 19.43 / CMake 3.31)
- Status: COMPLETE

## 1. Scope

### In Scope
- Comprehensive ownership and lifetime audit of the 2 GB host direct-memory physical pool (`g_phys_pool_base`, `g_phys_pool_offset`, `g_phys_pool_committed`, `g_phys_mutex`) in `src/hle/libkernel.cpp`.
- Analysis of interactions with `Memory::AdoptRange`, `Memory::ForgetResource`, `Memory::Shutdown()`, and the guest fault demand-commit path (`HLE::CommitPhysPool`).
- Establishment of teardown symmetry for `HLE::Shutdown()` via `HLE::ResetPhysPool()`.
- Creation of a characterization test verifying allocation, memory access, shutdown deallocation, and subsequent reinitialization.

### Out of Scope
- VideoOut detached VBlank thread restructuring (explicitly isolated to follow-up workstream).
- GPU Vulkan device / XInput static state.
- Module unloading architecture during active guest execution.

## 2. Current Reality
`src/hle/libkernel.cpp` lazily reserves a 2 GB host address range (`PHYS_POOL_SIZE = 2 GB`) via `VirtualAlloc(nullptr, PHYS_POOL_SIZE, MEM_RESERVE, PAGE_NOACCESS)` on the first call to `EnsurePhysPool()` (triggered by `sceKernelAllocateDirectMemory` or `sceKernelAllocateMainDirectMemory`). It adopts this range into the guest memory authority with `Memory::AdoptRange(g_phys_pool_base, PHYS_POOL_SIZE, Memory::PROT_NONE, /*committed=*/false, Memory::Owner::Hle, "phys-pool")`.

With `HLE::ResetPhysPool()` invoked during `HLE::Shutdown()`, the 2 GB host reservation is decommitted and released back to Windows via `VirtualFree(base, 0, MEM_RELEASE)`, `Memory::ForgetResource(base)` removes the unmanaged tracking record, and `g_phys_pool_base`, `g_phys_pool_offset`, and `g_phys_pool_committed` are cleanly reset to 0/default.

## 3. Ownership Matrix

| Resource / State Variable | Authoritative Owner | Creator / Allocator | Consumer | Reset / Free Responsibility | Lifecycle Class |
|---|---|---|---|---|---|
| `g_phys_pool_base` (Host 2 GB Reservation) | **HLE** (`src/hle/libkernel.cpp`) | `EnsurePhysPool` (`VirtualAlloc`) | `MapDirectMemoryCore`, `CommitPhysPool` | `HLE::ResetPhysPool` (`VirtualFree`) | Emulator-Instance |
| Committed Pages (`PHYS_COMMIT_CHUNK`) | **HLE** (`src/hle/libkernel.cpp`) | `EnsurePhysCommitted`, `CommitPhysPool` | Guest memory reads/writes | `VirtualFree(..., MEM_RELEASE)` | Emulator-Instance |
| Guest-visible Mappings | **Guest Process / HLE** | `sceKernelMapDirectMemory(2)` | Guest CPU / GPU | `Kernel::Shutdown` / `HLE::Shutdown` | Guest-Process |
| `g_phys_pool_offset` (Bump Cursor) | **HLE** (`src/hle/libkernel.cpp`) | `AllocateDirectMemory` | Direct-memory allocations | `HLE::ResetPhysPool` (reset to `0x10000`) | Guest-Process |
| `g_phys_pool_committed` (Commit Boundary) | **HLE** (`src/hle/libkernel.cpp`) | `EnsurePhysCommitted` | Chunk allocation tracking | `HLE::ResetPhysPool` (reset to 0) | Guest-Process |
| Region Table Record (`Region::Owner::Hle`) | **Memory** (`src/memory/memory.cpp`) | `Memory::AdoptRange` | `Memory::Query`, `IsValidGuestPointer` | `Memory::ForgetResource` | Emulator-Instance |

## 4. Resource Graph

```text
Guest sceKernelAllocateDirectMemory
    │
    ▼ (allocates 0-based offset from g_phys_pool_offset)
Physical Pool Bump Allocator
    │
    ▼ (commits 16MB chunks via EnsurePhysCommitted)
Host Committed Memory (VirtualAlloc MEM_COMMIT)
    │
    ▼ (mapped to guest VA via sceKernelMapDirectMemory)
Guest-Visible Address (target = g_phys_pool_base + phys_offset)
    │
    ▼ (registered in Memory region table as unmanaged Owner::Hle)
Memory Manager Tracking (Memory::AdoptRange)
    │
    ▼
HLE Subsystem Ownership (authoritative owner of the 2 GB host reservation)
```

## 5. Shutdown Dependency Graph

```text
Kernel::Shutdown()
  ├── Worker threads terminated via CpuCore::Shutdown()
  └── Module & guest metadata cleared
        │
        ▼
HLE::Shutdown()
  ├── Unmap thunk page
  ├── Clear symbol registry & stats
  ├── ResetLibcHeap()
  └── ResetPhysPool() ◄── [NEW SYMMETRIC RESET]
        ├── 1. Memory::ForgetResource(g_phys_pool_base) (untracks unmanaged region)
        ├── 2. VirtualFree(g_phys_pool_base, 0, MEM_RELEASE) (returns 2 GB to OS)
        └── 3. Reset g_phys_pool_base = 0, g_phys_pool_offset = 0x10000, g_phys_pool_committed = 0
              │
              ▼
Memory::Shutdown()
  ├── Removes guest fault VEH
  ├── Releases managed ranges and 1 GB direct pool
  └── Clears region & free range tables
```

## 6. Answers to Architectural Ownership Questions

1. **Who owns the host reservation?**
   `HLE` (`src/hle/libkernel.cpp`). It directly called `VirtualAlloc(nullptr, PHYS_POOL_SIZE, MEM_RESERVE, PAGE_NOACCESS)`.
2. **Who owns committed pages?**
   `HLE` (`src/hle/libkernel.cpp`). Releasing the host reservation via `VirtualFree(p, 0, MEM_RELEASE)` decommits and releases all pages atomically.
3. **Who owns guest-visible mappings?**
   Guest process / `HLE`.
4. **Who owns individual allocations from the pool?**
   Guest process (which receives 0-based offsets).
5. **Who is responsible for releasing the pool?**
   `HLE` (`HLE::Shutdown()` calling `ResetPhysPool()`).
6. **Can any mapping still reference the pool during HLE shutdown?**
   No. `Kernel::Shutdown()` and `CpuCore::Shutdown()` terminate all guest worker threads and tear down guest mappings before `HLE::Shutdown()` executes.
7. **Does `Memory::` own any portion of this pool?**
   No. `Memory::` only tracks it as an adopted region (`managed = false, owner = Owner::Hle`).
8. **Can the pool survive between emulator instances?**
   No. It must be cleanly released to avoid orphaning 2 GB host address space and stale allocation offsets.
9. **Is the pool process-lifetime or emulator-instance state?**
   It is **emulator-instance / guest-process state**.

## 7. Evidence

### SPECIFIED
- `architecture/RUNTIME_LIFECYCLE.md` specifies that `HLE` is torn down before `Memory`, and `HLE::Shutdown()` must release the physical pool.
- `src/memory/memory.cpp` specifies that adopted ranges (`managed = false`) are explicitly ignored by `Memory::Shutdown()` so their creator must release them.

### IMPLEMENTED
- `src/hle/libkernel.cpp` implements `EnsurePhysPool()`, `EnsurePhysCommitted()`, `IsPhysPoolAddress()`, `CommitPhysPool()`, and `ResetPhysPool()`.
- `src/hle/hle.cpp` calls `HLE::ResetPhysPool()` in `HLE::Shutdown()`.

### VERIFIED
- `TestHlePhysicalPoolLifecycleAndTeardown()` in `tests/libkernel_file_tests.cpp` verifies allocation, access, shutdown teardown (confirming MEM_FREE and memory query clearance), and cleanly reallocates in a second session.

## 8. Findings

### Finding 1: 2 GB Host Reservation Leak on Session Shutdown
- **Severity**: High
- **Evidence**: `g_phys_pool_base` was allocated via Win32 `VirtualAlloc(..., MEM_RESERVE)` and adopted into `Memory::`, but neither `HLE::Shutdown()` nor `Memory::Shutdown()` released it.
- **Impact**: Multi-session execution leaked 2 GB of virtual address space per run.
- **Resolution**: Implemented `HLE::ResetPhysPool()` invoked from `HLE::Shutdown()`.

### Finding 2: Unmanaged Region Dangling in Memory Region Table
- **Severity**: Medium
- **Evidence**: If the host memory were freed without calling `Memory::ForgetResource(g_phys_pool_base)`, `Memory::g_regions` retained a dangling pointer entry.
- **Impact**: `Memory::Query` could falsely report `Owner::Hle` for reallocated host pages.
- **Resolution**: Called `Memory::ForgetResource(g_phys_pool_base)` prior to `VirtualFree`.

## 9. Claims vs Reality

| Claim | Specification | Code Evidence | Test Evidence | Runtime Evidence | Status |
|---|---|---|---|---|---|
| **HLE Owns Physical Pool Host Reservation** | HLE reserves 2 GB directly | `libkernel.cpp:69-74` | Confirmed | Verified | **VERIFIED** |
| **Physical Pool Released on HLE::Shutdown()** | 2 GB freed and offsets reset | `libkernel.cpp:115-140`, `hle.cpp:401` | `libkernel_file_tests.cpp:294-350` | Verified in CTest & CLI | **VERIFIED** |

