# PCSX5 Runtime Lifecycle Architecture

## 1. Overview
The PCSX5 emulator runtime lifecycle governs the transition of subsystems through:
```text
  pcsx5_init()
       ↓
    Runtime / Execution
       ↓
  pcsx5_shutdown()
       ↓
     Reset
       ↓
  pcsx5_init() (Re-initialization)
```

## 2. Subsystem Lifecycle & Ownership Matrix

| Subsystem | Init Owner | Init Order | Runtime Owner | Shutdown Owner | Shutdown Order | Reset API | Global State | Threads | External Resources |
|---|---|---|---|---|---|---|---|---|---|
| **ConfigService** | `core_api.cpp` / `pcsx5_init` | 1 | `ConfigService` singleton (`config.cpp`) | `core_api.cpp` / `pcsx5_shutdown` | 7 (Post-persistence) | `ConfigService::Reset()` | `g_dir`, `g_global`, `g_per_title`, `g_initialized` | None | Filesystem JSON config files (`pcsx5_config/*.json`) |
| **Diagnostics** | `core_api.cpp` / `SubsystemRegistry` | 2 | `Diagnostics` namespace (`diagnostics.cpp`) | Process-lifetime | N/A | `ResetCrashReport()` | `g_mutex`, `g_crash`, `g_crash_present`, `g_handler_installed`, callbacks, `g_bundle_dir` | None | Win32 SEH filter, CRT invalid param/purecall/abort hooks |
| **Logging** | `core_api.cpp` / `SubsystemRegistry` | 3 | `LogConfig` (`log.cpp`) | Process-lifetime | N/A | `ClearRecentLogEntries()` | Sinks, `g_ring`, `g_file_stream`, category levels, dedup state | None | Log file streams, stderr/stdout |
| **Memory** | `SubsystemRegistry` (`Memory::Initialize`) | 4 | `Memory` subsystem (`memory.cpp`) | `SubsystemRegistry` (`Memory::Shutdown`) | 4 (Reverse order) | `Memory::Shutdown()` + `Memory::Initialize()` | `g_regions`, `g_free_ranges`, `g_pool_base`, `g_pool_used`, `g_pool_ok`, `g_pool_free`, `g_write_ranges`, `g_fault_veh` | None | Win32 VEH handle, 8 GB VirtualAlloc direct pool / guest mappings |
| **HLE** | `SubsystemRegistry` (`HLE::Initialize`) | 5 | `HLE` namespace (`hle.cpp` + module HLEs) | `SubsystemRegistry` (`HLE::Shutdown`) | 3 (Before Memory) | `HLE::Shutdown()` + `ResetLibcHeap()` + `ResetPhysPool()` + `ResetVideoOut()` | `g_symbol_registry`, `g_id_index`, `g_stats`, `g_stubbed_ids`, `g_thunk_page_base`, `g_stop_requested`, `g_guest_crashed`, `g_heap_bump/end/free`, `g_phys_pool_base`, `g_mutexes`, `g_conds`, `g_rwlocks`, `g_loaded_sysmodules`, `g_vo_ports`, `g_vblank_started`, `g_vblank_stop`, `g_vblank_thread` | Managed joinable VBlank pump thread (`g_vblank_thread`), WASAPI audio thread | 1 MB thunk page, 2 GB physical pool, 16 MB libc heap chunks, WASAPI/XAudio2/waveOut handles |
| **Kernel** | `SubsystemRegistry` (`Kernel::Initialize`) | 6 | `Kernel` subsystem (`kernel.cpp`) | `SubsystemRegistry` (`Kernel::Shutdown`) | 2 (Before HLE) | `Kernel::Shutdown()` | `g_threads`, `g_veh_handler`, `g_prev_exception_filter`, `g_guest_tls`, `g_guest_segments`, `g_loaded_modules`, `g_loaded_module_tls_index`, `g_tls_block_cache`, `g_tls_block_va`, `g_prx_modules`, `g_prx_loading`, `g_module_graph`, `g_main_module_copy`, `g_app0_dir`, `g_savedata_dir`, `g_fd_table`, `g_next_fd`, `g_brk_base`, `g_brk_current` | `g_heartbeat_thread`, guest OS worker threads | Win32 VEH handle, heartbeat event, 256 KB guest TLS, host file/socket handles |
| **CPU Core** | `Kernel::Initialize` / `CpuCore::Initialize` | Linked with Kernel | `CpuCore` (`cpu.cpp`) | `CpuCore::Shutdown()` | Linked with Kernel | `CpuCore::Shutdown()` | `g_threads`, `g_next_thread_id`, `g_custom_syscalls`, `g_initialized` | Guest OS threads | Host thread HANDLEs, wake event HANDLEs, guest stacks & TLS |
| **GPU** | `SubsystemRegistry` (`GPU::Initialize`) | 7 | `GPU` subsystem (`vulkan_backend.cpp`) | `SubsystemRegistry` (`GPU::Shutdown`) | 1 (First in teardown) | `GPU::Shutdown()` | `g_window`, `g_hwnd`, `g_vk`, `g_vk_ready`, `g_vk_pixels`, `g_dib_buffer`, `g_pad_state`, `g_keyboard_buttons`, `g_xinput_dll`, `g_XInputGetState`, `g_XInputSetState`, `g_xinput_inited`, `g_fullscreen`, `g_fb_width/height/format`, Boot state | Render / window message pump | GLFW window, Win32 HWND, Vulkan device/swapchain, XInput DLL |
| **Loader** | On-demand in `pcsx5_load` | Post-init | `Loader` namespace (`elf.cpp`, `pkg.cpp`, `pkg_ps5.cpp`, `param_json.cpp`) | Stateless / Kernel managed | N/A | None | `g_current_title_id` | None | Open ELF/PKG file streams |
| **Media Decoders** | On-demand per stream | Post-init | `Media` namespace (`media/`) | Stream destructor | N/A | Destructor | `g_ff`, `g_ff_loaded`, `g_ff_once` | Stream worker threads (`snd_player.cpp`) | FFmpeg DLLs, bink2w64.dll, waveOut handles |
| **Lua / SubsystemRegistry** | `core_api.cpp` / `RunDefaultInit` | Orchestrator | `SubsystemRegistry::Instance()` (`lua_init.cpp`) | `core_api.cpp` / `pcsx5_shutdown` | Orchestrator | `TeardownAll()` | `m_subsystems`, `m_init_order`, `m_initialized`, `m_resolved`, `g_lua_state` | None | Lua C state (`lua_State*`) |
| **Core API / Session** | `pcsx5_init` | Root entry | `g_state` (`core_api.cpp`) | `pcsx5_shutdown` | Root exit | `g_state = CoreState{}` + `ConfigService::Reset()` | `g_state`, `g_paused` | Main guest thread | Persisted summary reports |

---

## 3. Dependency Graphs

### Initialization Dependency Order
```text
ConfigService (1)
  ├──> Diagnostics (2) (reads crash.bundle_dir)
  ├──> Logging (3) (reads logging.file_path, json_output, log levels)
  └──> Memory (4) (initializes 8GB direct pool & guest fault VEH)
         ├──> HLE (5) (requires Memory for thunk page allocation)
         │      └──> Kernel (6) (requires Memory for guest TLS, HLE for phys pool & syscalls)
         │             └──> GPU (7) (requires Memory for FB, HLE for videoout, Kernel for threads)
```

### Teardown Dependency Order (Reverse Order)
```text
GPU (1) — Destroys swapchain, Vulkan context, GLFW window, DIB buffer, XInput
  ↓
Kernel (2) — Stops heartbeat, shuts down TlsPatch, closes FDs, unmaps guest TLS, removes VEH/SEH, clears threads/modules
  ↓
HLE (3) — Unmaps thunk page, clears symbol registry, resets guest heap, resets sync maps, releases phys pool
  ↓
Memory (4) — Removes guest fault VEH, VirtualFrees all manager allocations and 8GB pool, resets region tables
  ↓
Logging / Diagnostics — Process-lifetime sinks
  ↓
ConfigService — Reset in pcsx5_shutdown post-summary persistence
```

---

## 4. Resource Invariants

1. **One Resource, One Authoritative Owner**:
   - Guest Virtual Memory: strictly owned by `Memory` (`src/memory/memory.cpp`).
   - Guest Execution Threads: managed by `Kernel` / `CpuCore`.
   - Guest File Descriptors & OS Handles: managed by `Kernel::fd_table` (`src/kernel/fd_table.cpp`).
   - Host Presentation Window & Vulkan Device: strictly owned by `GPU` (`src/gpu/vulkan_backend.cpp`).
   - HLE Thunk Page & Heap: strictly owned by `HLE` (`src/hle/hle.cpp`, `src/hle/liblibc.cpp`).
2. **Reinitialization State Discipline**:
   - `ShutdownFdTable()` closes all open OS handles (`fd >= 3`) under non-recursive lock discipline and clears table state for subsequent re-initialization.
   - `HLE::Shutdown()` resets all execution state (`g_stop_requested`, `g_guest_crashed`, `g_trace`, `g_thunk_page_offset`, and libc guest heap bump pointers).
   - `ConfigService::Reset()` clears directory bindings and configuration dictionaries.
   - `Memory::Shutdown()` unregisters vectored exception handlers and releases all VirtualAlloc pools.
