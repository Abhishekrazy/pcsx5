# AUDIT: VideoOut / VBlank Pump Thread Ownership & Teardown Symmetry

- Date: 2026-08-24
- Baseline Commit: 9b2d8ff (main)
- Build: Release x64 (MSVC 19.43 / CMake 3.31)
- Status: COMPLETE

## 1. Scope

### In Scope
- Comprehensive ownership and lifetime audit of the VideoOut / VBlank pump thread (`VblankPumpLoop`, `g_vblank_thread`, `g_vblank_started`, `g_vblank_stop`, `g_vblank_edge_seq`, `g_vblank_mtx`, `g_vblank_cv`) and port registry state (`g_vo_ports`, `g_vo_mutex`, `g_next_vo_handle`, `g_vrr_active`) in `src/hle/libvideoout.cpp`.
- Establishing joinable thread ownership under HLE/VideoOut with immediate wake-on-stop condition variable synchronization.
- Ensuring teardown symmetry in `HLE::Shutdown()` by invoking `HLE::ResetVideoOut()`.
- Verifying complete absence of live detached emulator worker threads post-shutdown.
- Adding a characterization test verifying pump startup, edge progression, deterministic shutdown, host thread join, port state clearance, and clean re-initialization.

### Out of Scope
- GPU Vulkan presentation and swapchain pipeline restructuring.
- VideoOut format or flip mode semantics redesign.
- AudioOut WASAPI worker thread lifecycle (separate workstream).

## 2. Current Reality
In `src/hle/libvideoout.cpp`, when guest code opens a video-out port (`sceVideoOutOpen`) or waits on a vblank edge (`sceVideoOutWaitVblank`), `EnsureVblankPumpStarted()` launches `VblankPumpLoop` on a joinable host thread (`g_vblank_thread`).

With `HLE::ResetVideoOut()` invoked during `HLE::Shutdown()`:
1. `g_vblank_stop` is set to `true`, and `g_vblank_cv.notify_all()` immediately unblocks the wait condition.
2. `g_vblank_thread.join()` synchronously confirms thread exit without mutex deadlock.
3. `g_vo_ports` is cleared, `g_next_vo_handle` resets to `0x4000`, and `g_vblank_edge_seq`, `g_vrr_active`, `g_vblank_started`, `g_vblank_stop` are reset to clean initial states.
4. Subsequent emulator sessions reinitialize cleanly with 0 orphaned worker threads.

## 3. Ownership Matrix

| Resource / State Variable | Authoritative Owner | Creator / Allocator | Consumer | Reset / Free Responsibility | Lifecycle Class |
|---|---|---|---|---|---|
| `g_vblank_thread` (Host Thread) | **HLE / VideoOut** (`libvideoout.cpp`) | `EnsureVblankPumpStarted` | `VblankPumpLoop` | `HLE::ResetVideoOut` (`join()`) | Emulator-Instance |
| `g_vblank_stop` (Stop Flag) | **HLE / VideoOut** (`libvideoout.cpp`) | `libvideoout.cpp` | `VblankPumpLoop` | `HLE::ResetVideoOut` (set true, join, reset false) | Emulator-Instance |
| `g_vblank_started` (Start Gate) | **HLE / VideoOut** (`libvideoout.cpp`) | `EnsureVblankPumpStarted` | `EnsureVblankPumpStarted`, `ResetVideoOut` | `HLE::ResetVideoOut` (reset false) | Emulator-Instance |
| `g_vblank_edge_seq` (Edge Counter) | **HLE / VideoOut** (`libvideoout.cpp`) | `VblankPumpLoop` | `sceVideoOutWaitVblank` | `HLE::ResetVideoOut` (reset 0) | Guest-Process |
| `g_vo_ports` (Port Map) | **HLE / VideoOut** (`libvideoout.cpp`) | `sceVideoOutOpen` | `SubmitFlip`, `SignalVblank`, etc. | `HLE::ResetVideoOut` (`clear()`) | Guest-Process |
| `g_next_vo_handle` (Handle Generator) | **HLE / VideoOut** (`libvideoout.cpp`) | `sceVideoOutOpen` | `sceVideoOutOpen` | `HLE::ResetVideoOut` (reset `0x4000`) | Guest-Process |
| `g_vrr_active` (VRR Mode Flag) | **HLE / VideoOut** (`libvideoout.cpp`) | `VideoOutSetVrrMode` | `VblankPumpLoop`, `SubmitFlip` | `HLE::ResetVideoOut` (reset false) | Emulator-Instance |

## 4. Resource Graph

```text
sceVideoOutOpen / sceVideoOutWaitVblank
    │
    ▼
EnsureVblankPumpStarted()
    │
    ▼ (spawns managed joinable host thread)
std::thread g_vblank_thread (VblankPumpLoop)
    │
    ├─► 60 Hz Interval / CV wait (g_vblank_cv.wait_until)
    │     │
    │     ▼
    │   g_vblank_edge_seq.fetch_add(1)
    │     │
    │     ▼
    │   SignalVblank(port)
    │     │
    │     ▼
    │   SceKernelPostEvent(reg.equeue, kVideoOutInternalEventVblank)
    │
    ▼
HLE::ResetVideoOut() (Invoked during HLE::Shutdown)
    │
    ├── 1. g_vblank_stop.store(true)
    ├── 2. g_vblank_cv.notify_all() (immediate interrupt from wait_until)
    ├── 3. g_vblank_thread.join() (waits for deterministic exit)
    ├── 4. g_vo_ports.clear(), g_next_vo_handle = 0x4000
    └── 5. g_vblank_started = false, g_vblank_stop = false, g_vblank_edge_seq = 0
```

## 5. Shutdown Dependency Graph

```text
pcsx5_shutdown()
  │
  ├── GPU::Shutdown() (1)
  │     └── Destroys Vulkan backend, GLFW window, DIB buffers
  │
  ├── Kernel::Shutdown() (2)
  │     ├── CpuCore::Shutdown() (terminates guest worker threads)
  │     ├── ShutdownFdTable() (closes OS handles)
  │     └── Clears module/thread/segment metadata
  │
  ├── HLE::Shutdown() (3)
  │     ├── ResetVideoOut() ◄── [NEW SYMMETRIC RESET: Stops & joins VBlank thread, clears ports]
  │     ├── Unmap thunk page
  │     ├── Clear symbol registry & stats
  │     ├── ResetLibcHeap()
  │     └── ResetPhysPool()
  │
  └── Memory::Shutdown() (4)
        ├── Removes guest fault VEH
        └── Releases 8 GB direct pool and managed guest ranges
```

## 6. Answers to Architectural Ownership Questions

1. **Who owns the thread?**
   **HLE / VideoOut** (`src/hle/libvideoout.cpp`).
2. **Who owns the stop signal?**
   **HLE / VideoOut** (`g_vblank_stop` in `libvideoout.cpp`).
3. **Who owns the state accessed by the thread?**
   **HLE / VideoOut** owns `g_vo_ports` and `g_vblank_edge_seq`. Cross-subsystem notifications call `SceKernelPostEvent` in `libkernel_sync.cpp`.
4. **Who starts it?**
   `EnsureVblankPumpStarted()` in `src/hle/libvideoout.cpp` upon first port creation or vblank wait.
5. **Who stops it?**
   `HLE::ResetVideoOut()` called from `HLE::Shutdown()`.
6. **Who waits for it?**
   `HLE::ResetVideoOut()` via `g_vblank_thread.join()`.
7. **Who destroys its dependencies?**
   Subsystem teardown sequence (`HLE::Shutdown()` before `Memory::Shutdown()`).

## 7. Synchronization Model & Race Protection

- **Immediate Wakeup**: Replacing `std::this_thread::sleep_until` with `g_vblank_cv.wait_until(lk, next_edge, [&]{ return g_vblank_stop.load(); })` allows `ResetVideoOut()` to wake the thread with 0 ms sleep latency.
- **Mutex Hierarchy**: `g_vblank_mtx` is locked briefly only to signal the condition variable, and unlocked before `g_vblank_thread.join()` is called. `g_vo_mutex` is locked only after the thread has fully joined. This completely eliminates lock inversion and deadlocks.
- **Guest Waiters**: `sceVideoOutWaitVblank` checks `g_vblank_stop` in its `wait_for` predicate, ensuring any guest thread waiting on a vblank edge unblocks immediately during shutdown.

## 8. Evidence

### SPECIFIED
- `architecture/RUNTIME_LIFECYCLE.md` specifies that emulator-instance worker threads must not outlive `pcsx5_shutdown()`.
- Thread lifecycle contract: START -> RUN -> STOP REQUEST -> THREAD EXIT -> JOIN / CONFIRM EXIT -> RESOURCE DESTRUCTION.

### IMPLEMENTED
- `src/hle/hle.h`: Declared `EnsureVblankPumpStarted()`, `ResetVideoOut()`, `IsVblankPumpRunning()`.
- `src/hle/libvideoout.cpp`: Implemented joinable `g_vblank_thread`, CV-based interruptible loop in `VblankPumpLoop()`, `ResetVideoOut()`, `IsVblankPumpRunning()`.
- `src/hle/hle.cpp`: Hooked `ResetVideoOut()` into `HLE::Shutdown()`.

### VERIFIED
- `TestVideoOutLifecycleAndTeardown()` in `tests/hle_phase3_tests.cpp` (PASSED).
- Full CTest suite (45/45 suites passed in 61.78s).
- Dreaming Sarah (PPSA02929) and Brotato (PPSA21564) headless execution confirmed runtime parity.

## 9. Findings

### Finding 1: Detached VBlank Thread Survives Subsystem Teardown
- **Severity**: High
- **Evidence**: `EnsureVblankPumpStarted()` called `.detach()` on the thread and relied on `std::atexit`.
- **Impact**: Thread kept running across emulator sessions, accessing destroyed equeues and memory.
- **Resolution**: Store `std::thread g_vblank_thread`, implement `HLE::ResetVideoOut()`, and join the thread during `HLE::Shutdown()`.

### Finding 2: Uninterruptible Sleep Delayed Thread Shutdown
- **Severity**: Medium
- **Evidence**: `std::this_thread::sleep_until(next_edge)` could not be interrupted by stop signals.
- **Impact**: Up to 16.6ms shutdown lag and race conditions during fast restart cycles.
- **Resolution**: Use `g_vblank_cv.wait_until` with a stop predicate.

## 10. Claims vs Reality

| Claim | Specification | Code Evidence | Test Evidence | Runtime Evidence | Status |
|---|---|---|---|---|---|
| **VBlank Pump Thread is Joinable & Owned by HLE** | HLE manages thread object & join | `libvideoout.cpp:92,165-175` | `hle_phase3_tests.cpp:605-645` | Verified | **VERIFIED** |
| **VBlank Thread Exits & Joins Deterministically on Shutdown** | Immediate CV wakeup, 0 live worker threads post-shutdown | `libvideoout.cpp:138-155, 178-195` | `hle_phase3_tests.cpp:640-660` | Verified | **VERIFIED** |
| **VideoOut Ports & Counters Reset Symmetrically** | `g_vo_ports` cleared, handles & seq reset | `libvideoout.cpp:197-206` | `hle_phase3_tests.cpp:655-675` | Verified | **VERIFIED** |
