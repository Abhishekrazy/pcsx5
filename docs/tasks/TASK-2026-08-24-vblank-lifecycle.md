# TASK: VideoOut / VBlank Pump Thread Ownership & Teardown Symmetry

- Date: 2026-08-24
- Baseline Commit: 9b2d8ff (main)
- Branch: main
- Status: COMPLETE

## 1. Task
Audit and correct VideoOut / VBlank pump thread (`VblankPumpLoop`, `g_vblank_thread`, `g_vblank_started`, `g_vblank_stop`, `g_vblank_edge_seq`, `g_vblank_mtx`, `g_vblank_cv`) ownership and teardown symmetry so that the thread is joinable, owned exclusively by HLE/VideoOut, wakes up immediately upon shutdown without sleeping, and cleanly exits before underlying subsystem resources are destroyed.

## 2. Baseline
- Prior to fix, `src/hle/libvideoout.cpp` spawned `VblankPumpLoop` as a detached thread (`std::thread(VblankPumpLoop).detach()`).
- No handle was retained, preventing joinable synchronization during `pcsx5_shutdown()`.
- The loop used uninterruptible `std::this_thread::sleep_until()`, introducing sleep latency and potential race conditions.
- Stale port mappings and `g_vblank_started` survived shutdown, causing state pollution across multiple emulator runs.

## 3. Plan
1. Audit single authoritative ownership: confirm `HLE` is the sole owner of the pump thread and VideoOut port registries.
2. Establish dependency graph: VBlank pump thread -> HLE event posting (`libkernel_sync.cpp`) -> VideoOut ports.
3. Design interruptible synchronization: replace `std::this_thread::sleep_until` with `g_vblank_cv.wait_until` predicated on `g_vblank_stop`.
4. Hold `g_vblank_thread` as joinable `std::thread`, removing `.detach()` and `std::atexit`.
5. Implement `HLE::ResetVideoOut()`: set `g_vblank_stop = true`, notify condition variable, join thread, clear ports, and reset state variables.
6. Hook `ResetVideoOut()` at the start of `HLE::Shutdown()` in `src/hle/hle.cpp`.
7. Add characterization test `TestVideoOutLifecycleAndTeardown()` in `tests/hle_phase3_tests.cpp`.
8. Verify with full CTest suite (45/45 suites) and real-title headless regressions (PPSA02929 and PPSA21564).

## 4. Scope Boundary

### In Scope
- `src/hle/hle.h`: Declarations for `ResetVideoOut()`, `EnsureVblankPumpStarted()`, `IsVblankPumpRunning()`.
- `src/hle/libvideoout.cpp`: Joinable thread management, CV-based wait loop, stop predicate, `ResetVideoOut()` implementation.
- `src/hle/hle.cpp`: Calling `ResetVideoOut()` in `HLE::Shutdown()`.
- `tests/hle_phase3_tests.cpp`: `TestVideoOutLifecycleAndTeardown()` characterization test.
- `docs/audits/AUDIT-2026-08-24-vblank-lifecycle.md`: Ownership audit record.
- `docs/tasks/TASK-2026-08-24-vblank-lifecycle.md`: Task completion record.

### Out of Scope
- GPU Vulkan presentation and swapchain pipeline restructuring.
- VideoOut format or flip mode semantics redesign.
- AudioOut WASAPI worker thread lifecycle (separate workstream).

## 5. Architecture Impact
- **Owner**: HLE subsystem (`src/hle/libvideoout.cpp`).
- **Contracts**: Invariant "ONE THREAD -> ONE AUTHORITATIVE OWNER" established. START -> RUN -> STOP REQUEST -> THREAD EXIT -> JOIN / CONFIRM EXIT -> RESOURCE DESTRUCTION strictly enforced.
- **ADR Impact**: Enforces `architecture/RUNTIME_LIFECYCLE.md` lifecycle symmetry.
- **Invariants**: 0 live emulator-owned worker threads post-shutdown; lock hierarchy prevents deadlocks (`g_vblank_mtx` unlocked before joining `g_vblank_thread`).

## 6. Implementation

| File | Change | Reason |
|---|---|---|
| `src/hle/hle.h` | Declared `void EnsureVblankPumpStarted();`, `void ResetVideoOut();`, `bool IsVblankPumpRunning();` in `HLE` namespace. | Expose lifecycle control interface. |
| `src/hle/libvideoout.cpp` | Stored `g_vblank_thread` handle without `.detach()`; converted `VblankPumpLoop` to `g_vblank_cv.wait_until` with `g_vblank_stop` predicate; implemented `ResetVideoOut()` and `IsVblankPumpRunning()`; checked stop flag in `WaitVblankImpl`. | Enable deterministic joinable shutdown and clean state reset. |
| `src/hle/hle.cpp` | Added `ResetVideoOut()` call at start of `HLE::Shutdown()`. | Symmetrically stop & join VBlank thread prior to unmapping thunks and tearing down libc/phys pool. |
| `tests/hle_phase3_tests.cpp` | Added `TestVideoOutLifecycleAndTeardown()` and called it from `main()`. | Validate pump startup, edge generation, clean join on reset, port clearance, and re-initialization. |

## 7. Tests

| Test | Command | Result | Evidence |
|---|---|---|---|
| `hle_phase3_tests` | `.\build\Release\hle_phase3_tests.exe` | PASSED | `[TEST] VideoOut / VBlank pump lifecycle and teardown symmetry` (0.01s) |
| `Full CTest Suite` | `ctest --test-dir build -C Release --output-on-failure` | PASSED | 45/45 suites passed (100%) in 61.78s |

## 8. Runtime Validation

| Title | Title ID | Boot Stage | Result | Notes |
|---|---|---|---|---|
| Dreaming Sarah | PPSA02929 | In-Game / HLE Loop | PASSED | Zero boot signature change; HLE dispatches and `sceKernelUsleep` loop intact |
| Brotato | PPSA21564 | Engine Init / Exception Loop | PASSED | Zero boot signature change; VEH exception handling and module linking intact |

## 9. Claims vs Reality

| Claim | Specification | Code Evidence | Test Evidence | Runtime Evidence | Status |
|---|---|---|---|---|---|
| **VBlank Pump Thread is Joinable & Owned by HLE** | HLE manages thread object & join | `libvideoout.cpp:92,165-175` | `hle_phase3_tests.cpp:605-645` | Verified | **VERIFIED** |
| **VBlank Thread Exits & Joins Deterministically on Shutdown** | Immediate CV wakeup, 0 live worker threads post-shutdown | `libvideoout.cpp:138-155, 178-195` | `hle_phase3_tests.cpp:640-660` | Verified | **VERIFIED** |
| **VideoOut Ports & Counters Reset Symmetrically** | `g_vo_ports` cleared, handles & seq reset | `libvideoout.cpp:197-206` | `hle_phase3_tests.cpp:655-675` | Verified | **VERIFIED** |
| **Zero Regression on Existing Test Suites** | All 45 CTest suites pass | 45/45 passing | CTest output log (61.78s) | Verified | **VERIFIED** |

## 10. Verification
- Independent verifier: CTest test runner + MSVC 19.43 Release build + headless CLI runner for PPSA02929 & PPSA21564.
- Verdict: PASS.

## 11. Git State
- Modified files:
  - `src/hle/hle.h`
  - `src/hle/hle.cpp`
  - `src/hle/libvideoout.cpp`
  - `tests/hle_phase3_tests.cpp`
  - `architecture/RUNTIME_LIFECYCLE.md`
- Added / Updated documentation:
  - `docs/audits/AUDIT-2026-08-24-vblank-lifecycle.md`
  - `docs/tasks/TASK-2026-08-24-vblank-lifecycle.md`

## 12. Remaining Issues

### Blocking
None for VideoOut / VBlank pump thread lifecycle.

### Non-Blocking
- GPU XInput static flag in `src/gpu/vulkan_backend.cpp` is not reset on `GPU::Shutdown()`.
- AudioOut WASAPI worker thread lifecycle requires audit.

### Architectural Debt
None introduced.
