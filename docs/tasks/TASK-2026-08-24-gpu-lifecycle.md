# TASK: GPU Platform Resource Ownership & Teardown Symmetry

- Date: 2026-08-24
- Status: COMPLETED
- Workstream: Subsystem Lifecycle Symmetry
- Author: AI Agent

## 1. Goal
Audit and resolve lifecycle symmetry issues in the GPU subsystem (`src/gpu/vulkan_backend.cpp`), specifically fixing `xinput_inited` static local state retention, ensuring complete GPU instance-level state cleanup on `GPU::Shutdown()`, and verifying multi-session reinitialization stability.

## 2. Changes Made

### A. GPU Subsystem (`src/gpu/vulkan_backend.cpp`)
- Promoted `xinput_inited` from function-local static inside `GPU::PollEvents()` to file-scope `static bool g_xinput_inited = false;`.
- Guarded and updated `InitializeXInput()` to mark `g_xinput_inited = true;`.
- Eagerly invoked `InitializeXInput()` during `GPU::Initialize()`.
- Updated `GPU::Shutdown()` to reset all GPU subsystem emulator-instance state:
  - `g_xinput_inited = false;`
  - `g_xinput_dll = nullptr; g_XInputGetState = nullptr; g_XInputSetState = nullptr;`
  - `g_pad_state = PadButtonState{ 0, 127, 127, 127, 127, 0, 0, {0, 0} };` and `g_keyboard_buttons = 0;` (under `g_pad_mutex`)
  - `g_fullscreen = false; g_saved_x = 0; g_saved_y = 0; g_saved_w = 0; g_saved_h = 0;`
  - `g_fb_width = 1920; g_fb_height = 1080; g_fb_format = 0;`
  - `g_headless = false; g_embed_mode = false; g_window_created_cb = nullptr; g_window_created_cb_user = nullptr;`
  - `StopInputRecording(); g_recorder_frame.store(0, std::memory_order_relaxed);`
  - `g_dib_buffer.clear(); g_vk_pixels.clear();`

### B. Unit & Lifecycle Tests (`tests/hle_phase3_tests.cpp`)
- Added `#include "gpu/gpu.h"`.
- Implemented `TestGpuLifecycleAndTeardown()`:
  - Validates Session 1: `GPU::Initialize()` -> `GPU::PollEvents()` -> Pad state read -> `GPU::SetFramebufferConfig()` -> `GPU::Shutdown()` -> Pad state reset verification.
  - Validates Session 2: `GPU::Initialize()` -> `GPU::PollEvents()` -> XInput re-initialization verification -> `GPU::Shutdown()`.

### C. Architecture Documentation (`architecture/RUNTIME_LIFECYCLE.md`)
- Updated GPU subsystem row in the Lifecycle & Ownership Matrix to document `g_xinput_inited`, window geometry resets, and complete teardown state list.

## 3. Verification & Evidence

1. **Focused Unit Test**:
   - `ctest --test-dir build -C Debug -R hle_phase3 --verbose`
   - **Result**: PASS (1.01s). Confirmed 2 full cycles of GPU subsystem initialization, pad event polling, XInput loading, and clean shutdown.
2. **Full Test Suite**:
   - `ctest --test-dir build -C Debug --output-on-failure`
   - **Result**: 45/45 test suites passed (100%).
3. **Real Game Regressions**:
   - `Dreaming Sarah (PPSA02929)`: Expected execution milestones and unhandled exception baseline verified without regressions.
   - `Brotato (PPSA21564)`: Expected execution milestones and null-call exception baseline verified without regressions.

## 4. Git State
- `src/gpu/vulkan_backend.cpp` (modified)
- `tests/hle_phase3_tests.cpp` (modified)
- `architecture/RUNTIME_LIFECYCLE.md` (modified)
- `docs/audits/AUDIT-2026-08-24-gpu-lifecycle.md` (new)
- `docs/tasks/TASK-2026-08-24-gpu-lifecycle.md` (new)
