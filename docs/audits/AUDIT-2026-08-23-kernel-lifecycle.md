# AUDIT: Kernel Module & Thread Registry Teardown Symmetry

- Date: 2026-08-23
- Baseline Commit: 9b2d8ff (main)
- Build: Release x64 (MSVC 19.43 / CMake 3.31)
- Status: ACTIVE

## 1. Scope

### In Scope
- Comprehensive audit of all retained static and global state in the `Kernel` subsystem (`src/kernel/kernel.cpp`, `src/kernel/memory.cpp`, `src/kernel/thread.cpp`, `src/kernel/fd_table.cpp`, `src/kernel/tls_patch.cpp`).
- Audit of guest thread management, host `HANDLE` lifetime, wake events, and coordination between `Kernel` and `CpuCore` (`src/cpu/cpu.cpp`).
- Audit of module registration (`g_loaded_modules`), module graph (`g_module_graph`), PRX state (`g_prx_modules`), and dynamic TLS caches (`g_tls_block_cache`).
- Characterization test for Kernel initialization -> state mutation -> shutdown -> reinitialization.
- Surgical correction of `Kernel::Shutdown()`, `ShutdownGuestMemory()`, and `CpuCore::Shutdown()`.

### Out of Scope
- Dynamic module unloader implementation (runtime module unload is prohibited by Rule 08 without evidence).
- HLE direct physical memory pool deallocation (managed in separate HLE follow-up).
- GPU Vulkan/XInput static flags (managed in separate GPU follow-up).
- Vblank pump thread refactoring (managed in separate HLE follow-up).

## 2. Current Reality
Prior to this audit, `Kernel::Shutdown()` only stopped the heartbeat thread, tore down `TlsPatch`, shut down the file descriptor table, called an empty `ShutdownGuestMemory()`, removed the VEH handler, restored the unhandled exception filter, and unmapped `g_guest_tls`. It did not invoke `CpuCore::Shutdown()` and left all thread registries, module registries, guest segments metadata, PRX caches, search directories, path mappings, and the BRK heap cursor intact in memory.

## 3. System Inventory

| Component / State Variable | Owner | Creator | Consumer | Reset Point | Lifecycle Class |
|---|---|---|---|---|---|
| `g_threads` (`kernel.cpp`) | Kernel | `Kernel::Initialize`, `RegisterThread` | `ResolveGuestThreadPointer`, VEH | `Kernel::Shutdown` | Guest-Process |
| `CpuCore::g_threads` (`cpu.cpp`) | CpuCore | `CpuCore::CreateThread`, `RegisterExistingThread` | `GetThreadById`, `Suspend/Wake` | `CpuCore::Shutdown` | Guest-Process |
| `g_loaded_modules` (`kernel.cpp`) | Kernel | `RegisterLoadedModule` | `FindModuleForAddr`, `sceKernelGetModuleInfo` | `Kernel::Shutdown` | Guest-Process |
| `g_loaded_module_tls_index` | Kernel | `RegisterLoadedModule` | `ResolveDynamicTls` | `Kernel::Shutdown` | Guest-Process |
| `g_tls_block_cache` / `_va` | Kernel | `ResolveDynamicTls` | `ResolveDynamicTls` | `Kernel::Shutdown` | Guest-Process |
| `g_prx_modules` / `_loading` | Kernel | `LoadPrxModuleRecursive` | PRX linking, exports | `Kernel::Shutdown` | Guest-Process |
| `g_module_graph` | Kernel | `LoadPrxModuleRecursive` | TopoSort, load order | `Kernel::Shutdown` | Guest-Process |
| `g_main_module_copy` / `_retained` | Kernel | `Execute` | VEH crash symbolizer | `Kernel::Shutdown` | Guest-Process |
| `g_guest_segments` | Kernel | `Execute` | VEH segment matching | `Kernel::Shutdown` | Guest-Process |
| `g_module_resolver` search dirs | Kernel | `ConfigureModuleResolver` | PRX file lookup | `Kernel::Shutdown` | Emulator-Instance |
| `g_app0_dir` / `g_savedata_dir` | Kernel | `SetApp0Directory`, `SetSaveDataDirectory` | `TranslateGuestPath` | `Kernel::Shutdown` | Guest-Process |
| `g_brk_base` / `g_brk_current` | Kernel | `SetBreak` | `GetBreak`, `SetBreak` | `ShutdownGuestMemory` | Guest-Process |
| `g_fd_table` | Kernel | `AllocateFd`, `DuplicateFd` | POSIX/HLE file I/O | `ShutdownFdTable` | Guest-Process |
| `g_veh_handler` / `_filter` | Kernel | `Kernel::Initialize` | Host exception dispatcher | `Kernel::Shutdown` | Emulator-Instance |
| `g_guest_tls` | Kernel | `Kernel::Initialize` | Guest TLS pointer | `Kernel::Shutdown` | Emulator-Instance |

## 4. Evidence

### VERIFIED
- `CpuCore::Shutdown()` cleanly terminates running guest threads via `::TerminateThread` and closes host thread and wake event handles.
- `ShutdownGuestMemory()` does not free memory ranges directly (respecting `Memory::` single authority), but must clear its internal `g_brk_base` and `g_brk_current` variables.
- Clearing `g_loaded_modules` and `g_threads` during `Kernel::Shutdown()` ensures subsequent game sessions start with an empty module and thread registry.

### OPERATIONALLY CONFIRMED
- CTest suite (45/45 tests passing) and real-title boots (PPSA02929, PPSA21564) confirm that clearing Kernel state on shutdown does not regress single-run or continuous execution.

## 5. Findings

### Finding 1: CpuCore Teardown Ordering & Invocation
- **Severity**: Critical
- **Evidence**: `Kernel::Shutdown()` did not invoke `CpuCore::Shutdown()`. Furthermore, if `TlsPatch` or `Memory` was dismantled before `CpuCore::Shutdown()`, live guest threads could fault on invalid TLS slots or unmapped heap pages.
- **Files**: `src/kernel/kernel.cpp`, `src/cpu/cpu.cpp`
- **Invariant**: Worker threads must be stopped before memory/exception-handling infrastructure is torn down.
- **Runtime Impact**: Leaked host OS threads and potential use-after-free crashes during shutdown.
- **Confidence**: 100%

### Finding 2: Module Registry & PRX State Retention
- **Severity**: High
- **Evidence**: `g_loaded_modules`, `g_prx_modules`, and `g_module_graph` retained metadata from previous game runs across `pcsx5_shutdown()`.
- **Files**: `src/kernel/kernel.cpp`
- **Invariant**: Module metadata must be isolated per guest process lifetime.
- **Runtime Impact**: In multi-game execution, `FindModuleForAddr` matches stale addresses from previous titles.
- **Confidence**: 100%

### Finding 3: Stale BRK Heap Cursor
- **Severity**: High
- **Evidence**: `g_brk_base` and `g_brk_current` in `src/kernel/memory.cpp` were not reset during `ShutdownGuestMemory()`.
- **Files**: `src/kernel/memory.cpp`
- **Invariant**: Memory manager is the single guest VA authority; kernel cursors over manager mappings must reset on session teardown.
- **Runtime Impact**: Re-initialization returns a dangling pointer into previously unmapped virtual address space.
- **Confidence**: 100%

## 6. Architecture

### Ownership
- **Guest Virtual Memory**: Authoritative owner is `Memory::`. Kernel only maintains cursor metadata (`g_brk_base/current`) and segment metadata (`g_guest_segments`).
- **Threads**: `CpuCore` owns the thread objects, host handles, and wake events. `Kernel::` provides the guest OS abstraction.
- **Modules**: `Loader` parses ELF/PKG formats; `Kernel` tracks loaded module descriptors and resolves dynamic TLS.

### Teardown Dependency Ordering
```text
Kernel::Shutdown()
  ├── 1. StopHeartbeat()
  ├── 2. CpuCore::Shutdown() (Terminates guest threads, closes handles)
  ├── 3. TlsPatch::Shutdown() (Invalidates stubs, frees TLS index)
  ├── 4. ShutdownFdTable() (Closes open file/socket handles)
  ├── 5. ShutdownGuestMemory() (Resets BRK cursors)
  ├── 6. Exception Handlers (Removes VEH, restores filter)
  ├── 7. Guest TLS (Unmaps TLS block, resets context)
  └── 8. Metadata Clear (Clears threads, modules, PRX, graph, paths)
```

## 7. Tests and Runtime Evidence

| Test | Command | Result | Evidence |
|---|---|---|---|
| `libkernel_file_tests` | `libkernel_file_tests.exe` | PASSED | Full lifecycle test with module, thread, BRK, and path resets |
| `Full CTest` | `ctest --test-dir build -C Release` | PASSED | 45/45 test suites passing |
| `PPSA02929` | `pcsx5_cli.exe --title-id=PPSA02929 ...` | PASSED | Headless boot verified with zero regression |
| `PPSA21564` | `pcsx5_cli.exe --title-id=PPSA21564 ...` | PASSED | Headless boot verified with zero regression |

## 8. Claims vs Reality

| Claim | Specification | Code Evidence | Test Evidence | Runtime Evidence | Status |
|---|---|---|---|---|---|
| **Kernel Subsystem Retained State Audited** | Identify all static/global state | `kernel.cpp`, `memory.cpp`, `cpu.cpp` | Audit table created | Code inspection verified | **VERIFIED** |
| **CpuCore Teardown Linked to Kernel** | `CpuCore::Shutdown()` executed in safe order | `Kernel::Shutdown()` calls `CpuCore::Shutdown()` first | Thread handles closed without crash | Verified in lifecycle test | **VERIFIED** |
| **Module & Thread State Cleared on Shutdown** | All registries reset on `Kernel::Shutdown()` | Explicit clears in `kernel.cpp` & `memory.cpp` | `TestKernelLifecycleAndTeardownSymmetry` | Verified | **VERIFIED** |
| **Zero Regression on Real Titles** | PPSA02929 and PPSA21564 execute identically | Execution logs match baseline | CTest 45/45 pass | Bot/headless tests pass | **VERIFIED** |

## 9. Remaining Risks
- HLE direct memory physical pool (`g_phys_pool_base`, 2 GB) in `src/hle/libkernel.cpp` is not yet freed during `HLE::Shutdown()`.
- GPU `xinput_inited` static boolean in `src/gpu/vulkan_backend.cpp` is not reset during `GPU::Shutdown()`.
- Videoout detached VBlank thread needs joinable synchronization on shutdown.

## 10. Recommended Next Task
**HLE Direct Memory Physical Pool and VBlank Thread Teardown Symmetry**: Correct `HLE::Shutdown()` to deallocate `g_phys_pool_base` (2 GB) and join `VblankPumpLoop` thread cleanly.
