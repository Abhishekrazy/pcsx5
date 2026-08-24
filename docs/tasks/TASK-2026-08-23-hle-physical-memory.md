# TASK: HLE Direct Memory Physical Pool Ownership & Teardown

- Date: 2026-08-23
- Baseline Commit: 9b2d8ff (main)
- Branch: main
- Status: COMPLETE

## 1. Task
Audit and correct HLE direct memory physical pool (`g_phys_pool_base`, `g_phys_pool_offset`, `g_phys_pool_committed`, `g_phys_mutex`) ownership and teardown symmetry so that the 2 GB host reservation is cleanly released back to Windows during `HLE::Shutdown()`, untracked from `Memory::g_regions`, and allocation cursors reset without violating the Memory ownership contract.

## 2. Baseline
- Baseline CTest pass rate: 45/45 suites passed (100%).
- Prior to fix, `src/hle/libkernel.cpp` allocated a 2 GB reservation (`g_phys_pool_base`) via `VirtualAlloc(nullptr, PHYS_POOL_SIZE, MEM_RESERVE, PAGE_NOACCESS)` and adopted it into `Memory::` as unmanaged (`managed = false, owner = Owner::Hle`).
- Neither `HLE::Shutdown()` nor `Memory::Shutdown()` freed the reservation, resulting in a 2 GB address space leak per emulator instance and stale allocation offsets surviving into future sessions.

## 3. Plan
1. Conduct Phase 1 ownership audit: trace the complete lifetime of `g_phys_pool_base`, prove single authoritative ownership (HLE), and verify Memory contract (`managed = false` means owner must release).
2. Build resource graph and shutdown dependency hierarchy.
3. Declare `HLE::ResetPhysPool()` in `src/hle/hle.h` and implement in `src/hle/libkernel.cpp`.
4. In `ResetPhysPool()`, atomically capture and reset `g_phys_pool_base`, `g_phys_pool_offset`, `g_phys_pool_committed`, untrack from `Memory::` via `Memory::ForgetResource(base)`, and release memory with `VirtualFree(base, 0, MEM_RELEASE)`.
5. Invoke `HLE::ResetPhysPool()` inside `HLE::Shutdown()` in `src/hle/hle.cpp`.
6. Add characterization test `TestHlePhysicalPoolLifecycleAndTeardown()` in `tests/libkernel_file_tests.cpp`.
7. Verify with full CTest suite, focused test harness, and headless title runs (PPSA02929 and PPSA21564).

## 4. Scope Boundary

### In Scope
- `src/hle/libkernel.cpp`: `ResetPhysPool()` implementation and `SceKernelAllocateDirectMemory` symbol export.
- `src/hle/hle.h` / `src/hle/libkernel_file.h`: Declarations for `ResetPhysPool` and `SceKernelAllocateDirectMemory`.
- `src/hle/hle.cpp`: Invocation of `ResetPhysPool()` in `HLE::Shutdown()`.
- `tests/libkernel_file_tests.cpp`: `TestHlePhysicalPoolLifecycleAndTeardown()` characterization test.
- `docs/audits/AUDIT-2026-08-23-hle-physical-memory.md`: Durable ownership audit.

### Out of Scope
- VideoOut detached VBlank thread restructuring (explicitly isolated to follow-up workstream).
- GPU Vulkan device / XInput static state.
- Module unloading architecture during active guest execution.

## 5. Architecture Impact

- **Owner**: HLE subsystem (`src/hle/libkernel.cpp`).
- **Contracts**: Invariant "ONE RESOURCE -> ONE AUTHORITATIVE OWNER" maintained. `Memory::` does not manage or free unmanaged adopted ranges; HLE explicitly frees what it creates.
- **ADR Impact**: Enforces `architecture/RUNTIME_LIFECYCLE.md` lifecycle symmetry.
- **Invariants**: Teardown symmetry restored; 2 GB host reservation returned to OS; mutex lock hierarchy preserved (`g_phys_mutex` released before `Memory::ForgetResource` to avoid lock inversion).

## 6. Implementation

| File | Change | Reason |
|---|---|---|
| `src/hle/hle.h` | Declared `void ResetPhysPool();` in `HLE` namespace. | Expose physical pool teardown interface to `HLE::Shutdown()`. |
| `src/hle/libkernel_file.h` | Declared `u64 SceKernelAllocateDirectMemory(const GuestArgs& args);`. | Allow direct-memory allocation testing from test harness. |
| `src/hle/libkernel.cpp` | Implemented `ResetPhysPool()`: under `g_phys_mutex`, snapshots base/offset/committed, zeroes them, calls `Memory::ForgetResource(base)`, and calls `VirtualFree(base, 0, MEM_RELEASE)`. | Return 2 GB reservation to OS and clean up region table. |
| `src/hle/libkernel.cpp` | Exported `SceKernelAllocateDirectMemory(const GuestArgs& args)`. | Expose direct memory allocation handler for characterization tests. |
| `src/hle/hle.cpp` | Added `ResetPhysPool()` call inside `HLE::Shutdown()`. | Symmetrically tear down physical pool during HLE shutdown. |
| `tests/libkernel_file_tests.cpp` | Added `TestHlePhysicalPoolLifecycleAndTeardown()`. | Exercise allocation, memory access, shutdown deallocation verification (`MEM_FREE`), and fresh-session re-allocation. |

## 7. Tests

| Test | Command | Result | Evidence |
|---|---|---|---|
| `libkernel_file_tests` | `.\build\Release\libkernel_file_tests.exe` | PASSED | `hle physical pool lifecycle and teardown symmetry: OK` (0.03s) |
| `Full CTest Suite` | `ctest --test-dir build -C Release --output-on-failure` | PASSED | 45/45 suites passed (100%) in 6.81s |

## 8. Runtime Validation

| Title | Title ID | Boot Stage | Result | Notes |
|---|---|---|---|---|
| Dreaming Sarah | PPSA02929 | In-Game / HLE Loop | PASSED | Zero boot signature change; HLE dispatches and `sceKernelUsleep` loop intact |
| Brotato | PPSA21564 | Engine Init / Exception Loop | PASSED | Zero boot signature change; VEH exception handling and module linking intact |

## 9. Claims vs Reality

| Claim | Specification | Code Evidence | Test Evidence | Runtime Evidence | Status |
|---|---|---|---|---|---|
| **HLE Owns Physical Pool Host Reservation** | HLE allocates 2 GB directly via `VirtualAlloc` | `libkernel.cpp:69-74` | `VirtualQuery` verifies reservation | Verified | **VERIFIED** |
| **Physical Pool Released on HLE::Shutdown()** | 2 GB freed and offsets reset | `libkernel.cpp:115-140`, `hle.cpp:401` | `TestHlePhysicalPoolLifecycleAndTeardown` verifies `MEM_FREE` & reinitialization | Verified in CTest & CLI | **VERIFIED** |
| **Zero Memory Contract Violation** | Memory only untracks unmanaged range via `ForgetResource` | `libkernel.cpp:131`, `memory.cpp:914` | `Memory::Query` returns `Owner::None` after shutdown | Verified in test | **VERIFIED** |
| **Zero Regression on Existing Test Suites** | All 45 CTest suites pass | 45/45 passing | CTest output log (6.81s) | Verified | **VERIFIED** |

## 10. Verification
- Independent verifier: CTest test runner + MSVC 19.43 Release build + headless CLI runner for PPSA02929 & PPSA21564.
- Verdict: PASS.

## 11. Git State
- Modified files:
  - `src/hle/hle.h`
  - `src/hle/hle.cpp`
  - `src/hle/libkernel.cpp`
  - `src/hle/libkernel_file.h`
  - `tests/libkernel_file_tests.cpp`
- Added / Updated documentation:
  - `docs/audits/AUDIT-2026-08-23-hle-physical-memory.md`
  - `docs/tasks/TASK-2026-08-23-hle-physical-memory.md`

## 12. Remaining Issues

### Blocking
None for HLE physical memory pool lifecycle.

### Non-Blocking
- GPU XInput static flag in `src/gpu/vulkan_backend.cpp` is not reset on `GPU::Shutdown()`.
- Videoout detached VBlank thread needs joinable synchronization on shutdown.

### Architectural Debt
None introduced.
