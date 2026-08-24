# AUDIT: GPU Platform Resource Ownership & Teardown Symmetry

- Date: 2026-08-24
- Baseline Commit: 9b2d8ff (main)
- Build: Debug/Release x64 (MSVC 19.43 / CMake 3.31)
- Status: COMPLETE

## 1. Scope

### In Scope
- Comprehensive inventory and ownership audit of all GPU subsystem resources, static variables, and platform adapters in `src/gpu/vulkan_backend.cpp`, `src/gpu/vk_context.cpp`, `src/gpu/vk_present.cpp`, `src/gpu/vk_draw.cpp`, and `src/gpu/dualsense_hid.h`.
- Platform boundary analysis across GLFW, Vulkan, Win32 GDI/DWM, XInput, and DualSense HID.
- Investigation and elimination of `xinput_inited` static local variable lifecycle bug in `GPU::PollEvents()`.
- Establishing complete teardown symmetry for all GPU instance-level state (`g_xinput_inited`, `g_pad_state`, `g_keyboard_buttons`, `g_fullscreen`, `g_saved_x/y/w/h`, `g_fb_width/height/format`, `g_headless`, `g_embed_mode`, `g_window_created_cb`, input recorder).
- Multi-session reinitialization verification and full regression testing.

### Out of Scope
- GPU Vulkan rendering pipeline, swapchain presentation architecture, or GCN/RDNA2 emulation redesign.
- Shader recompiler or SPIR-V translation changes.
- Frame pacing, VRR, or presentation sync redesign.
- DualSense native HID thread architecture modifications.

## 2. Current Reality
In `src/gpu/vulkan_backend.cpp`, `xinput_inited` was previously declared as a function-local static variable inside `GPU::PollEvents()`. Although `GPU::Shutdown()` unloaded the XInput library via `FreeLibrary(g_xinput_dll)` and reset function pointers to `nullptr`, `xinput_inited` remained `true` across sessions because function-local statics have process lifetime. On any subsequent session within the same host process, `GPU::PollEvents()` observed `xinput_inited == true`, skipped `InitializeXInput()`, leaving `g_XInputGetState == nullptr` and permanently disabling controller support.

Furthermore, several GPU subsystem instance variables (`g_pad_state`, `g_keyboard_buttons`, `g_fullscreen`, `g_saved_x/y/w/h`, `g_fb_width/height/format`, `g_headless`, `g_embed_mode`) were not reset in `GPU::Shutdown()`, causing state to leak between consecutive runs.

Following the audit and remediation:
1. `xinput_inited` has been promoted to file-scope `g_xinput_inited` and is explicitly reset to `false` in `GPU::Shutdown()`.
2. `InitializeXInput()` is eagerly called during `GPU::Initialize()` and guarded by `g_xinput_inited`.
3. `GPU::Shutdown()` synchronously resets all pad states, button bitmasks, window geometries, framebuffer properties, callbacks, and stops input recording.
4. Consecutive sessions now initialize, poll events, present, and tear down with 100% symmetry.

## 3. Ownership Matrix

| Resource / State Variable | Authoritative Owner | Creator / Allocator | Consumer | Reset / Free Responsibility | Lifecycle Class |
|---|---|---|---|---|---|
| `g_window` (GLFWwindow*) | **GPU** (`vulkan_backend.cpp`) | `glfwCreateWindow()` | Window event pump, Vulkan surface | `GPU::Shutdown` (`glfwDestroyWindow`) | Emulator-Instance |
| `g_hwnd` (HWND) | **GPU** (`vulkan_backend.cpp`) | `glfwGetWin32Window()` | Win32 GDI blit, UI embedding | `GPU::Shutdown` (`g_hwnd = nullptr`) | Emulator-Instance |
| `g_vk` (VkContext*) | **GPU** (`vulkan_backend.cpp`) | `VkContextCreate()` | `VkPresent*`, `VkDraw*` | `GPU::Shutdown` (`VkContextDestroy`) | Emulator-Instance |
| `g_vk_ready` (Present Flag) | **GPU** (`vulkan_backend.cpp`) | `VkPresentInitialize()` | `RenderFrame()` | `GPU::Shutdown` (`g_vk_ready = false`) | Emulator-Instance |
| `g_vk_pixels` (BGRA Framebuffer) | **GPU** (`vulkan_backend.cpp`) | `ConvertFramebufferToBgra()` | Vulkan texture upload | `GPU::Shutdown` (`clear()`) | Emulator-Instance |
| `g_dib_buffer` (DIB Backing Store) | **GPU** (`vulkan_backend.cpp`) | `GPU::Initialize()` | Boot screen, GDI fallback | `GPU::Shutdown` (`clear()`) | Emulator-Instance |
| `g_xinput_dll` (HMODULE) | **GPU / Platform** (`vulkan_backend.cpp`) | `LoadLibraryA()` | `InitializeXInput()` | `GPU::Shutdown` (`FreeLibrary`) | Emulator-Instance |
| `g_XInputGetState` / `SetState` | **GPU / Platform** (`vulkan_backend.cpp`) | `GetProcAddress()` | `PollEvents()`, `SetPadVibration()` | `GPU::Shutdown` (set `nullptr`) | Emulator-Instance |
| `g_xinput_inited` (Init Gate) | **GPU** (`vulkan_backend.cpp`) | `InitializeXInput()` | `PollEvents()`, `GPU::Initialize()` | `GPU::Shutdown` (set `false`) | Emulator-Instance |
| `g_pad_state` (Controller State) | **GPU** (`vulkan_backend.cpp`) | `PollEvents()` | `GetCurrentPadState()` | `GPU::Shutdown` (reset defaults) | Emulator-Instance |
| `g_keyboard_buttons` (Key Mask) | **GPU** (`vulkan_backend.cpp`) | `KeyCallback()` | `PollEvents()` | `GPU::Shutdown` (set `0`) | Emulator-Instance |
| `g_fullscreen` / `g_saved_x/y/w/h` | **GPU** (`vulkan_backend.cpp`) | `ToggleFullscreen()` | `ToggleFullscreen()` | `GPU::Shutdown` (reset geometry) | Emulator-Instance |
| `g_fb_width/height/format` | **GPU** (`vulkan_backend.cpp`) | `SetFramebufferConfig()` | `RenderFrame()` | `GPU::Shutdown` (reset defaults) | Emulator-Instance |
| `g_headless` / `g_embed_mode` | **GPU** (`vulkan_backend.cpp`) | `GPU::Initialize()` / API | `GPU::Initialize()` | `GPU::Shutdown` (reset `false`) | Emulator-Instance |
| `g_input_recorder` / frame | **GPU** (`vulkan_backend.cpp`) | `StartInputRecording()` | `PollEvents()` | `GPU::Shutdown` (`StopInputRecording`) | Emulator-Instance |
| `Boot()` Singleton & Log | **GPU** (`vulkan_backend.cpp`) | `SetBootStatus()` | Boot screen renderer | `GPU::Shutdown` (reset `status`, `log`) | Emulator-Instance |
| `DualSense` Reader Thread | **GPU / Platform** (`dualsense_hid.h`) | `DualSense::EnsureStarted()` | `PollEvents()`, `SetPadVibration()` | Host background HID poll loop | Process-Lifetime |

## 4. Teardown Hierarchy

```text
pcsx5_shutdown()
  │
  ├── 1. GPU::Shutdown()
  │     ├── VkDrawShutdown()
  │     │     ├── FlushBatch() & DeviceWaitIdle()
  │     │     ├── Destroy pipelines, layouts, descriptors, passes, framebuffers
  │     │     ├── Destroy texture & guest storage buffers & device memory
  │     │     ├── Destroy scalar ring, index ring, staging ring, command pool
  │     │     └── Save & destroy VkPipelineCache; g_ds = DrawState{}
  │     │
  │     ├── VkPresentShutdown(g_vk)
  │     │     ├── DeviceWaitIdle()
  │     │     ├── Destroy swapchain, semaphores, fences, present command pool
  │     │     └── g_ps = PresentState{}
  │     │
  │     ├── VkContextDestroy(g_vk)
  │     │     ├── Destroy device, Win32 surface, Vulkan instance
  │     │     ├── FreeLibrary(vulkan-1.dll)
  │     │     └── delete g_vk; g_vk = nullptr, g_vk_ready = false
  │     │
  │     ├── glfwDestroyWindow(g_window); g_window = nullptr, g_hwnd = nullptr
  │     ├── glfwTerminate()
  │     ├── Reset Boot() status and log; g_boot_active = true
  │     ├── FreeLibrary(g_xinput_dll); g_xinput_dll = nullptr, g_xinput_inited = false
  │     ├── Reset g_pad_state, g_keyboard_buttons, g_fullscreen, g_fb_width/height/format
  │     ├── StopInputRecording()
  │     └── Clear g_dib_buffer & g_vk_pixels
  │
  ├── 2. Kernel::Shutdown()
  │     ├── CpuCore::Shutdown()
  │     ├── ShutdownFdTable()
  │     └── Reset module, thread, TLS, and BRK state
  │
  ├── 3. HLE::Shutdown()
  │     ├── ResetVideoOut() (stops & joins VBlank pump thread, clears ports)
  │     ├── ResetPhysPool() (unmaps 2 GB direct physical pool)
  │     ├── ResetLibcHeap() (unmaps 16 MB heap chunks)
  │     └── Unmap thunk page & clear symbol registry
  │
  └── 4. Memory::Shutdown()
        ├── Removes guest fault VEH handler
        └── VirtualFree 8 GB direct pool and guest address spaces
```

## 5. Answers to Architectural Questions

1. **Why was XInput initialization guarded?**
   It was guarded to avoid redundant `LoadLibraryA` and `GetProcAddress` calls on every per-frame input poll.
2. **What resource is initialized?**
   `g_xinput_dll` (`HMODULE`) and function pointers `g_XInputGetState`, `g_XInputSetState`.
3. **Is that resource destroyed during GPU shutdown?**
   Yes, `GPU::Shutdown()` unloads `g_xinput_dll` via `FreeLibrary` and clears the function pointers.
4. **Does the flag represent process-global or emulator-instance state?**
   Because the DLL handle and function pointers are released during `GPU::Shutdown()`, the initialization state is strictly **emulator-instance state**.
5. **What was the reinitialization bug?**
   Because `xinput_inited` was a function-local static variable, it retained `true` across shutdown. On the next session, `PollEvents()` saw `xinput_inited == true`, skipped calling `InitializeXInput()`, and since `g_XInputGetState` was null, controller polling remained disabled for all subsequent sessions.
6. **How was it fixed?**
   `g_xinput_inited` was made a file-scope boolean, eagerly initialized in `GPU::Initialize()`, checked in `PollEvents()`, and reset to `false` in `GPU::Shutdown()`.

## 6. Verification Evidence

### Automated Unit Test: `hle_phase3_tests` (`TestGpuLifecycleAndTeardown`)
- Session 1: `GPU::Initialize()` -> `GPU::PollEvents()` -> Pad state verified (`left_analog = 127`, `buttons = 0`) -> `GPU::SetFramebufferConfig(1280, 720, 1)` -> `GPU::Shutdown()` -> Pad state verified reset.
- Session 2: `GPU::Initialize()` -> `GPU::PollEvents()` -> XInput re-loaded cleanly (`g_xinput_inited` active) -> Pad state verified (`left_analog = 127`) -> `GPU::Shutdown()`.
- Result: **PASS** (1.01s).

### Full CTest Suite
- 45/45 test suites executed and passed (100% pass rate).

### Real Title Validation
- **Dreaming Sarah** (`PPSA02929`):
  - Execution reached expected runtime milestones and unhandled exception termination without regressions.
- **Brotato** (`PPSA21564`):
  - Execution progressed through PRX loading and guest execution to expected null-call exception baseline with zero regressions.
