# TASK: Kernel Module & Thread Registry Teardown Symmetry

- Date: 2026-08-23
- Baseline Commit: 9b2d8ff (main)
- Branch: main
- Status: COMPLETE

## 1. Task
Audit and correct the Kernel subsystem teardown symmetry so that emulator-instance and guest-process state owned by Kernel does not survive `pcsx5_shutdown()`, and ensure `CpuCore::Shutdown()` is executed in correct dependency order (before destroying underlying memory/TLS/exception handlers).

## 2. Baseline
- Baseline CTest pass rate: 45/45 suites passed (100%).
- Prior to fix, `Kernel::Shutdown()` retained `g_threads`, `g_guest_segments`, `g_loaded_modules`, `g_loaded_module_tls_index`, `g_tls_block_cache`, `g_tls_block_va`, `g_prx_modules`, `g_prx_loading`, `g_module_graph`, `g_main_module_copy`, `g_app0_dir`, `g_savedata_dir`, and `g_brk_base/current`. It also omitted `CpuCore::Shutdown()`.

## 3. Plan
1. Audit all retained Kernel state variables and their authoritative owners and lifetimes.
2. Establish correct teardown sequence: `StopHeartbeat -> CpuCore::Shutdown -> TlsPatch::Shutdown -> ShutdownFdTable -> ShutdownGuestMemory -> Remove VEH/SEH -> Unmap guest TLS -> Clear module/thread/path/graph metadata`.
3. Add characterization test `TestKernelLifecycleAndTeardownSymmetry` in `tests/libkernel_file_tests.cpp`.
4. Implement surgical state clearance in `src/kernel/kernel.cpp`, `src/kernel/memory.cpp`, and `src/cpu/cpu.cpp`.
5. Run full CTest suite, focused tests, and real-title regression tests on PPSA02929 and PPSA21564.

## 4. Scope Boundary

### In Scope
- `src/kernel/kernel.cpp`: `Kernel::Shutdown()` implementation and static state lifecycle.
- `src/kernel/memory.cpp`: `ShutdownGuestMemory()` reset of `g_brk_base` and `g_brk_current`.
- `src/cpu/cpu.cpp`: `CpuCore::Shutdown()` reset of thread IDs.
- `tests/libkernel_file_tests.cpp`: `TestKernelLifecycleAndTeardownSymmetry` characterization test.

### Out of Scope
- Module unloading architecture during active guest execution.
- HLE direct physical memory pool deallocation (`src/hle/libkernel.cpp`).
- GPU XInput static flag and GLFW window teardown.
- Videoout detached thread restructuring.

## 5. Architecture Impact

- Owner: Kernel subsystem (`src/kernel/`) and CPU subsystem (`src/cpu/`).
- Contracts: Invariant "ONE RESOURCE -> ONE AUTHORITATIVE OWNER" maintained. Guest memory VA ownership remains strictly with `Memory::`; Kernel only manages its heap cursor metadata.
- ADR Impact: Enforces `architecture/RUNTIME_LIFECYCLE.md` contract.
- Invariants: Teardown symmetry restored; guest threads terminated before memory/VEH unmapping.

## 6. Implementation

| File | Change | Reason |
|---|---|---|
| `src/kernel/kernel.cpp` | Updated `Kernel::Shutdown()` to invoke `CpuCore::Shutdown()` and clear all module registries, graph, PRX sets, dynamic TLS caches, guest segments, threads, and path strings under proper mutexes. | Prevent stale state leaks across emulator sessions and prevent use-after-free by live worker threads. |
| `src/kernel/memory.cpp` | Reset `g_brk_base = 0` and `g_brk_current = 0` in `ShutdownGuestMemory()`. | Prevent subsequent sessions from indexing dangling unmapped virtual addresses. |
| `src/cpu/cpu.cpp` | Reset `g_next_thread_id = 1` and `g_current_thread_id = 0` in `CpuCore::Shutdown()`. | Clean thread ID numbering on re-initialization. |
| `tests/libkernel_file_tests.cpp` | Added `TestKernelLifecycleAndTeardownSymmetry()`. | Characterize and verify complete lifecycle, module lookup, path mapping, and BRK reset. |

## 7. Tests

| Test | Command | Result | Evidence |
|---|---|---|---|
| `libkernel_file_tests` | `build\Release\libkernel_file_tests.exe` | PASSED | 11/11 tests passed in 0.04s |
| `Full CTest Suite` | `ctest --test-dir build -C Release --output-on-failure` | PASSED | 45/45 suites passed (100%) in 7.19s |

## 8. Runtime Validation

| Title | Title ID | Boot Stage | Result | Notes |
|---|---|---|---|---|
| Dreaming Sarah | PPSA02929 | In-Game / HLE Loop | PASSED | Zero boot signature change; HLE dispatches intact |
| Brotato | PPSA21564 | Engine Init / Exception Loop | PASSED | Zero boot signature change; VEH exception handling intact |

## 9. Claims vs Reality

| Claim | Specification | Code Evidence | Test Evidence | Runtime Evidence | Status |
|---|---|---|---|---|---|
| **Kernel Module & Thread State Cleared on Shutdown** | All 14 identified state items reset | `kernel.cpp:507-555`, `memory.cpp:36-41` | `TestKernelLifecycleAndTeardownSymmetry` passed | Verified | **VERIFIED** |
| **CpuCore Shutdown Precedes Kernel De-registration** | Worker threads terminated before VEH/TLS/Memory teardown | `kernel.cpp:511` calls `CpuCore::Shutdown()` first | Thread handles closed without crash | Verified in lifecycle test | **VERIFIED** |
| **Zero Regression on Existing Test Suites** | All 45 CTest suites pass | 45/45 passing | CTest output log | Verified | **VERIFIED** |

## 10. Verification
- Independent verifier: CTest harness + MSVC build + Win32 headless test runner.
- Verdict: PASS.

## 11. Git State
- Modified files:
  - `src/kernel/kernel.cpp`
  - `src/kernel/memory.cpp`
  - `src/cpu/cpu.cpp`
  - `tests/libkernel_file_tests.cpp`
- Added documentation:
  - `docs/audits/AUDIT-2026-08-23-kernel-lifecycle.md`
  - `docs/tasks/TASK-2026-08-23-kernel-lifecycle.md`

## 12. Remaining Issues

### Blocking
None for Kernel subsystem lifecycle.

### Non-Blocking
- HLE direct memory physical pool (2 GB) in `src/hle/libkernel.cpp` is not freed on `HLE::Shutdown()`.
- GPU XInput static boolean in `src/gpu/vulkan_backend.cpp` is not reset on `GPU::Shutdown()`.
- Videoout detached VBlank thread needs joinable synchronization on shutdown.

### Architectural Debt
None introduced.

## 13. Recommended Next Task
**HLE Direct Memory Physical Pool and VBlank Thread Teardown Symmetry**: Correct `HLE::Shutdown()` to deallocate `g_phys_pool_base` (2 GB) and join `VblankPumpLoop` thread cleanly.
