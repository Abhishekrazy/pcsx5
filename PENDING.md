# Pending — prioritized work queue

Low/medium priority first; high-priority items broken into small,
independently verifiable pieces.

Legend: `[ ]` pending · `[~]` in progress · `[x]` done

---

## Phase H — H4 Menu Blocker Resolution (High Priority)

Context: splash renders correctly; after ~180 frames the guest enters a
content-load phase (no draws), then the run dies silently ~8-10 min in.

### H4 Menu Blockers

- [x] **H4.1 Construct runtime null variants (Crash A)**: Audit heap recycling / `HeapAllocLocked` zeroing for 0xb0 poison byte reuse during JSON parse. The sce::Json::Value default constructor sets type=kNull (0) but heap-recycled memory may retain stale bytes (0xb0 from AGC) at the type field offset.
  - [x] Fix: `HeapAllocLocked` now zeroes allocations (liblibc.cpp:101,114)
  - [x] Value constructors write type field explicitly (libjson.cpp)
- [x] **H4.2 VCRUNTIME140 access violation (Crash B)**: Audit AGC map/write paths (`d-6uF9sZDIU` / `memcpy` chain) for leaked host pointers (`0x7ff...`) written into guest memory.
  - [x] Add `Memory::IsValidGuestPointer()` utility
  - [x] Add guest-pointer validation to `SetIndirectPatchAddress` (d-6uF9sZDIU)
  - [x] Add guest-pointer validation to `DcbWaitRegMem` (address in PM4)
  - [x] SEH guards already cover memcpy/memmove paths in libkernel.cpp

- [~] **H4.3 Unwind & Exception diagnostics**: Verify VEH catch / C++ exception handler unwinding during menu transitions. The 8-10 min death may be host stack exhaustion in recursive VEH/TLS path.
  - [x] Add stack-depth guard (`VehRecursionGuard`, max depth 8) in VEH handler
  - [x] Heartbeat now logs VEH recursion depth + thread stack commit sizes
  - [ ] Verify heartbeat thread is still alive at T+8min (requires running game)

- [ ] **H4.4 Golden Frame Comparison**: Compare output menu rendering against reference dumps; fix format, tiling, and blend mismatches.
  - [ ] Capture PM4 stream at the first menu frame
  - [ ] Compare against SharpEmu golden reference
  - [ ] Fix any format/tiling mismatches in the Vulkan present path

---

## Phase A — Hardware Abstraction Layers (High Priority)

*Goal: every device subsystem uses a clean abstract interface so backends
can be swapped at runtime/config without modifying HLE code.*

### A1. Graphics Abstraction Layer (GAL)

Currently: GPU backend is Vulkan via volk (dynamic loading) + GLFW for windows.
GDI DIB fallback exists for boot screen. Renderer choice is hardcoded.

- [x] **A1.1 Define `GpuDevice` interface** (`src/gpu/gal.h`):
  - Initialize/Shutdown, swapchain, resource management
  - Pipeline creation, command submission, draw calls
  - Guest memory upload helpers, readback for golden frame testing
  - Null/headless device for unit testing
  - Backend factory with auto-probe

- [ ] **A1.2 Refactor existing Vulkan backend** into `VulkanDevice : GpuDevice`:
  - Move `vk_context.cpp` → adapter implementing the interface
  - Move `vk_present.cpp` → `VulkanSwapchain`
  - Move `vk_draw.cpp` → `VulkanCommandBuffer`
  - Keep shader compiler as shared component

- [ ] **A1.3 GDI/Software fallback** as `GdiDevice : GpuDevice`:
  - Refactor existing DIB present path into the interface
  - Add simple software rasterizer for debug/testing

- [ ] **A1.5 Runtime backend selection** via `gpu.backend` config:
  - Vulkan / D3D12 / GDI / Null with auto-probe priority

### A2. Audio Abstraction Layer (AAL)

Currently: 3 backends (WASAPI, XAudio2, waveOut) hardcoded in libaudioout.cpp
with platform-specific init inside HLE symbols. No abstract interface.

- [x] **A2.1 Define `AudioDevice` interface** (`src/hle/audio/audio_device.h`):
  - Open/Close/Output/SetVolume/Reset
  - Backend factory with auto-probe priority
  - Pacing-only null backend for headless use

- [ ] **A2.2 Refactor waveOut** into `WaveOutDevice : AudioDevice`
- [ ] **A2.3 Refactor WASAPI** into `WasapiDevice : AudioDevice`
- [ ] **A2.4 Refactor XAudio2** into `Xa2Device : AudioDevice`
- [ ] **A2.5 Add SDL Audio backend** (`SdlAudioDevice : AudioDevice`)
- [ ] **A2.7 Backend factory** with auto-probe: SDL → XAudio2 → WASAPI → waveOut → Pacing

### A3. Input Abstraction Layer (IAL)

Currently: GLFW keyboard mapping + XInput for controllers in `gpu.h/cpp`.
DualSense HID in `dualsense_hid.h`. No clean abstraction.

- [x] **A3.1 Define `InputBackend` interface** (`src/gpu/input/input_backend.h`):
  - Poll/SetRumble/SetTriggerEffect
  - InputMultiplexer for merging multiple backends
  - Null backend for testing

- [ ] **A3.2 Refactor GLFW keyboard** into `GlfwKeyboardInput : InputBackend`
- [ ] **A3.3 Refactor XInput** into `XInputBackend : InputBackend`
- [ ] **A3.4 Add SDL GameController** (`SdlGameController : InputBackend`)
- [ ] **A3.5 Refactor DualSense HID** into `DualSenseInput : InputBackend`
- [ ] **A3.6 Input multiplexer**: allow multiple backends simultaneously

### A4. Platform Abstraction Layer (PAL)

- [x] **A4.1-A4.5 Define `Platform` API** (`src/common/platform/platform.h`):
  - Memory: VirtualAlloc/VirtualFree/VirtualProtect (abstracts VirtualAlloc/mmap)
  - Thread: GetCurrentThreadId, SetThreadName
  - Dynamic libraries: LoadLibrary/GetProcAddress/FreeLibrary
  - Exception handling: InstallFaultHandler/RemoveFaultHandler
  - CPU feature detection: QueryCpuFeatures

- [x] **A4.6 Windows implementation** (`src/common/platform/platform_win32.cpp`):
  - Full Win32 backend with large-page support, VEH wrapper
  - CPUID-based feature detection (SSE4a/BMI/ABM/AVX/AVX2)

---

## Phase P — PS5 PKG Extraction & ELF Loading (Medium Priority)

*Goal: extract eboot.bin from PS5 fPKG packages without requiring a
retail PS5 console dump. Handle both fake-signed (fPKG) and detect
retail NPDRM with clear error messages.*

### P1. PS5 PKG Format Support in C++ Loader

- [x] **P1.1 PS5 PKG parser** (`src/loader/pkg_ps5.h/.cpp`):
  - Header layout (magic 0x7F464948, big-endian fields)
  - Entry table with {u32 id, u32 type/flags, u64 offset, u64 size, u64 pad}
  - File names embedded in first 256 bytes of entry data
  - PFS image offset/size from layout table @0x400
  - DRM type detection (fake/NPDRM)

- [x] **P1.2 PS5 fPKG entry decryption**:
  - AES-128-CBC per-entry decryption (same scheme as PS4)
  - Key derivation from content_id + passcode

- [x] **P1.3 Auto-detect PS4 vs PS5 PKG** (`ExtractAnyPkg` by magic):
  - CLI `--extract-pkg` can handle both formats
  - Dispatch by magic byte

### P2. PFS Image Extraction & Mounting

Both PS4 and PS5 PKGs contain a PFS (PlayStation File System) image.
The eboot.bin lives inside this image.

- [ ] **P2.1 PFS reader improvements** (`src/loader/pfs.cpp`):
  - Handle both PS4 and PS5 PFS variants
  - Extract individual files from PFS image
  - Support compressed PFS blocks if present
- [ ] **P2.2 Mount PFS from PKG** without full extraction:
  - Direct file read from PFS inside PKG
  - Map guest `/app0/` to PFS mount point
- [ ] **P2.3 PFS write support** (for savedata):
  - Already partially done; verify against PS5 format

### P3. SELF → ELF Extraction

PS5 executables use the SELF (Signed ELF) format wrapping the inner ELF.

- [ ] **P3.1 SELF parser** (`src/loader/self.h/.cpp`):
  - Parse SELF header, segment info, program headers
  - Extract inner ELF from SELF
  - Handle fake-signed (fPKG) SELF with known keys
  - Detect retail NPDRM SELF and provide clear message
- [ ] **P3.2 SELF decryption**:
  - AES-128-CTR decryption of SELF segments (fPKG key)
  - Key derivation from content_id + passcode
- [ ] **P3.3 Integration with ELF loader**:
  - Detect SELF format by magic (0x4F454C53 = "SCE\x7F" for PS4 SELFs; PS5 uses different magic)
  - Auto-extract inner ELF before passing to existing loader
  - Support both direct eboot.bin (decrypted ELF) and SELF-wrapped

### P4. PkgToolBox Integration

The existing `tools/PkgToolBox/` directory has a Python PS5 PKG parser.

- [ ] **P4.1 Audit PkgToolBox PS5 logic** for header/entry format correctness
- [ ] **P4.2 Port PS5-specific crypto** to C++ (key derivation, AES-CBC)
- [ ] **P4.3 Add CLI tool** `pcsx5 --extract-pkg <file.pkg> <outdir>` for PS5
- [ ] **P4.4 Document extraction workflow** for users

---

## Phase O — Performance Optimization (Medium Priority)

*Goal: improve emulation speed to reach real-time or better on modern
hardware, focusing on the highest-impact bottlenecks first.*

### O1. Memory System Optimization

- [ ] **O1.1 Large-page support** for guest memory:
  - Use `VirtualAlloc` with `MEM_LARGE_PAGES` when available
  - Reduces TLB pressure for the 16 GiB+ guest address space
  - Fall back to regular pages when large pages unavailable
- [ ] **O1.2 Direct-mapped guest memory**:
  - Map guest physical memory at a fixed host address
  - Eliminate translate() calls in hot paths
  - Use a single large VirtualAlloc reservation
- [ ] **O1.3 Fault-free page commits**:
  - Pre-commit commonly-used memory regions at boot
  - Reduce demand-commit fault overhead
- [ ] **O1.4 Guest write tracking optimization**:
  - Batch protection changes (currently one VirtualProtect per arm)
  - Use larger tracked region granularity

### O2. GPU Pipeline Optimization

- [ ] **O2.1 Command batching** (currently one submit+fence-wait per draw):
  - Batch multiple draws into a single command buffer
  - Use timeline semaphores for async submission
  - Target: 10-100 draws per submit
- [ ] **O2.2 Async pipeline compilation**:
  - Create VkPipeline on a background thread
  - Use VkPipelineCache for persistent caching
  - Pipeline-creation stall tracking in diagnostics
- [ ] **O2.3 Descriptor pooling & reuse**:
  - Pre-allocate descriptor pools per frame
  - Reuse descriptor sets across similar draws
  - Reduce per-frame descriptor allocation overhead
- [ ] **O2.4 Staging buffer ring**:
  - Pre-allocated ring of host-visible buffers
  - Upload guest texture data through the ring
  - Avoid per-upload allocation
- [ ] **O2.5 Shader cache disk format optimization**:
  - Versioned cache format with forward-compat
  - Cache sharing across titles with same shaders
  - Incremental cache updates

### O3. CPU/Threading Optimization

- [ ] **O3.1 Thread affinity & topology awareness**:
  - Pin guest worker threads to physical cores
  - Keep main thread on its own core
  - Respect CPU config `affinity_mask` setting
- [ ] **O3.2 Guest thread scheduling**:
  - Reduce pthread/mutex contention in guest sync primitives
  - Implement guest thread yield → host thread yield
  - Priority inheritance for guest mutexes
- [ ] **O3.3 VEH/TLS path optimization**:
  - Reduce contention on `g_thread_mutex` in the VEH hot path
  - Use a lock-free thread-local cache for TLS base lookup
  - Profile VEH overhead during content-load phase (H4.3)

### O4. Audio Pipeline Optimization

- [ ] **O4.1 Buffer ring refactoring**:
  - Shared ring buffer implementation across all backends
  - Reduce lock contention in the audio output path
- [ ] **O4.2 Zero-copy guest audio read**:
  - Map guest audio buffer directly for host playback
  - Avoid the `Memory::ReadBuffer` + conversion copy
  - Do conversion during DMA instead of during Output

### O5. Boot Time Optimization

- [ ] **O5.1 Parallel module loading**:
  - Load + relocate PRX modules in parallel
  - Dependency-respectful thread pool for module init
- [ ] **O5.2 Shader warmup from disk cache**:
  - Load and compile cached shaders on background thread
  - Reduce first-frame pipeline compilation stalls

---

## Phase S — System & Quality Improvements (Ongoing)

### S1. Build System & CI

- [ ] **S1.1 Split CMakeLists into per-subdirectory targets**:
  - `src/common/CMakeLists.txt`, `src/kernel/CMakeLists.txt`, etc.
  - Better incremental build times
- [ ] **S1.2 Add Clang-CL build configuration**:
  - Validate code with both MSVC and Clang-CL
  - Catch MSVC-specific bugs earlier
- [ ] **S1.3 CI: add GPU-less test runs**:
  - Run ctest on CI without Vulkan/GDI
  - Headless mode for unit tests
- [ ] **S1.4 CI: add PKG extraction test**:
  - Test with synthetic fPKG fixtures
  - Verify eboot.bin extraction

### S2. Testing & Diagnostics

- [ ] **S2.1 Expand test coverage**:
  - Add unit tests for new GAL/AAL/IAL interfaces
  - Add PS5 PKG parse-test with synthetic data
  - Add SELF extract-test with synthetic data
- [ ] **S2.2 Diagnostic improvements**:
  - Frame timing waterfall in debug overlay
  - Per-subsystem CPU usage breakdown
  - Memory allocation heatmap
- [ ] **S2.3 Crash bundle improvements**:
  - Include active config + title info
  - Include last N frames of trace ring
  - Include GPU pipeline cache on crash

## Phase V — Video Decoder Support (High Priority)

*Goal: play game cutscenes and UI videos encoded in common formats
used by PS5 titles — Bink 2, CRI Sofdec2 (USM), H.264/H.265,
VP9/AV1.  Each format gets a decoder backend behind a unified
VideoDecoder interface.*

### V1. Video Decoder Abstraction Layer (VDAL)

- [ ] **V1.1 Define `VideoDecoder` interface**:
  - `Open(url_or_data, format) -> bool`
  - `GetNextFrame() -> VideoFrame {pixels, timestamp, duration}`
  - `Seek(timestamp) -> bool`
  - `GetAudioTracks() -> tracks[]`  (for audio stream routing)
  - `GetVideoInfo() -> {width, height, fps, codec}`
  - `Close()`
- [ ] **V1.2 Define `VideoFrame` structure**:
  - Pixel data in GPU-friendly format (RGBA8, YUV420, NV12)
  - Timestamp + duration (for A/V sync)
  - Side data for HDR metadata when present
- [ ] **V1.3 Backend factory** with auto-probe:
  - Hardware decoder (DXVA/VDPAU) → FFmpeg → Bink2 → CRI

### V2. Bink 2 Video Support

Bink 2 (RAD Game Tools) is used by hundreds of games including many
PS5 titles. The SDK is royalty-free for developers.

- [ ] **V2.1 `Bink2Decoder : VideoDecoder`**:
  - Load Bink 2 `.bik2` files
  - Decode to RGBA8 frames via the Bink 2 SDK (RADTelemetry)
  - Frame-accurate seeking
- [ ] **V2.2 Bink 1 fallback** (`.bik` files used in PS4 back-compat):
  - Decode via the older Bink 1 format
- [ ] **V2.3 GPU upload integration**:
  - Upload decoded frames directly to Vulkan/D3D12 texture
  - Zero-copy path when possible (shared surfaces)

### V3. CRI Sofdec2 / USM Support

CRIWARE's movie format is heavily used in Japanese PS5 games (Namco,
Square Enix, Atlus, etc.). The `.usm` container wraps H.264 or H.265
video with ADPCM audio.

- [ ] **V3.1 `CriUsmDecoder : VideoDecoder`**:
  - Parse USM container headers and track metadata
  - Extract H.264/H.265 video stream
  - Extract ADPCM audio stream → route to audio device
- [ ] **V3.2 H.264/H.265 decoding**:
  - Use D3D11VA / DXVA hardware decoder on Windows
  - FFmpeg software fallback for cross-platform
- [ ] **V3.3 A/V sync**:
  - Timestamp-based sync with audio output device
  - Dropped-frame handling for performance

### V4. Generic Media Playback (libSceM4Player HLE)

PS5's `libSceM4Player` provides a standard media playback API.

- [ ] **V4.1 FFmpeg-based decoder**:
  - `FFmpegDecoder : VideoDecoder`
  - Supports H.264, H.265, VP9, AV1
  - Hardware acceleration via D3D11VA / Vulkan video
  - Cross-platform (Windows, Linux, macOS)
- [ ] **V4.2 Integrate with libScePlayer HLE**:
  - Route `scePlayerOpen` etc. to `FFmpegDecoder`
  - Map PS5 media API to generic decoder
- [ ] **V4.3 Subtitle overlay**:
  - Render subtitle bitmaps onto video frames
  - Support PGS, UTF-8 subtitle formats

### V5. Video Output Integration

- [ ] **V5.1 Video frame → GPU texture upload**:
  - Upload decoded YUV/RGBA frames to Vulkan or D3D textures
  - Use Vulkan samplers for color conversion (YUV→RGB)
- [ ] **V5.2 Video overlay compositing**:
  - Composite video over game rendering or boot screen
  - Handle letterbox/aspect-ratio correction
- [ ] **V5.3 A/V sync timing**:
  - Use guest clock for timestamp alignment
  - Frame-dropping policy based on decoder backlog

### S3. Documentation

- [ ] **S3.1 Architecture docs update**:
  - Document new abstraction layers (GAL/AAL/IAL/PAL/VDAL)
  - Add PKG extraction workflow docs
  - Update wiki with PS5 PKG format notes
- [ ] **S3.2 Developer guide**:
  - How to add a new backend
  - How to add abstraction for a new platform
  - Testing guide for hardware backends

---

## Phase D — DualSense HID Fixes & Improvements (High Priority)

*Goal: fix Bluetooth mic/speaker, LED RGB, and integrate the tester
UI into the button layout system for reuse in the emulator UI.*

### D1. Bluetooth Mic/Speaker Fix

- [ ] **D1.1 Audit Bluetooth output report format**:
  - BT output reports use HID report ID 0x31 (vs USB 0x02)
  - BT frames need: `0x31 | seq(1) | data(…) | crc32(4)`
  - USB output report length is ~63 bytes, BT is ~547 bytes (padded)
  - Check `g_out_report_len` is read correctly from device caps
- [ ] **D1.2 Fix HID output write for BT**:
  - Update `WriteOutputReport` in dualsense_hid.h to frame BT reports
    with sequence number + CRC32
  - BT sequence number must increment monotonically per report
  - CRC32 covers the full report (header + data)
- [ ] **D1.3 Verify mic/speaker routing**:
  - Mic audio: the DualSense input report contains 16-bit PCM mic data
    at offset ~34 (4 channels × 16-bit for directional mic array)
  - Speaker audio: the DualSense output report accepts haptics + speaker
    audio in the same payload
  - Route mic data to host audio input device, speaker data to output

### D2. Lightbar LED RGB Fix

- [ ] **D2.1 Audit output report byte offsets**:
  - USB output: `report[0]=0x02`, lightbar RGB at offset 0x06 (3 bytes R,G,B)
  - BT output: `report[0]=0x31`, lightbar RGB at offset 0x06 (3 bytes R,G,B)
  - Flag byte at offset 0x02 must have bit 0x02 set to enable lightbar writes
  - Verify byte order (the DualSense uses R-G-B, some clones use G-R-B)
- [ ] **D2.2 Add validation**:
  - Check `g_out_report_len` >= required offset before writing
  - Verify the enable flags are correctly OR'd in `SendOutputLocked`
  - Add debug logging of the actual bytes written for the lightbar
- [ ] **D2.3 Test both USB and BT paths**:
  - USB path: report id 0x02 → lightbar at offset 3-5 → flag at offset 2
  - BT path: report id 0x31 → lightbar at offset 7-9 → flag at offset 6
  - The `SendOutputLocked` function must dispatch to the right template

### D3. Tester UI → Button Layout Integration

- [ ] **D3.1 Create shared `ButtonLayout` component**:
  - Render PlayStation controller button shapes in correct positions
  - Circle (◯), Cross (✕), Square (□), Triangle (△)
  - D-pad, L1/R1, L2/R2 (trigger bars), L3/R3 (stick clicks)
  - Analog stick overlays showing current X/Y position
  - Touchpad with finger position circles
  - Gyro/Accel 3-axis readout bars
- [ ] **D3.2 Embed in ImGui**:
  - Create an `ImGui::ButtonLayout(state)` widget
  - Color each button green when pressed, white when idle
  - Show "tested" checkmark when each button has been seen pressed
- [ ] **D3.3 Embed in WPF UI**:
  - Port the ImGui layout to a WPF `UserControl` for the launcher
  - Bind controller state from the core API
- [ ] **D3.4 Replace standalone tester tools**:
  - Remove or redirect `dualsense_test` and `dualsense_visual` to
    the unified `--test-input` CLI mode that opens the ImGui tester

### D4. Player LED & Battery

- [ ] **D4.1 Wire player LEDs to user profile**:
  - Call `SetPlayerLeds` with bitmask matching active controller index
  - Update on profile switch
- [ ] **D4.2 Expose battery in UI**:
  - Read `Sample::battery_level` and `battery_charging`
  - Show in the debug overlay and libScePad HLE

---

## Phase R — Rendering Enhancements (High Priority)

*Goal: modern display features — VRR, FSR upscaling, HDR.*

### R1. Variable Refresh Rate (VRR)

- [ ] **R1.1 Vulkan VK_KHR_swapchain with VRR**:
  - Query monitor VRR support via DXGI (`IDXGIFactory5` /
    `DXGI_ADAPTER_FLAG2_SUPPORT_VARIABLE_REFRESH_RATE`)
  - Enable adaptive vsync with `VK_PRESENT_MODE_FIFO_RELAXED_EXT`
    or `VK_PRESENT_MODE_IMMEDIATE_EXT` + VRR
  - Config: `video.vrr = true|false`
- [ ] **R1.2 Tear-free presentation**:
  - D3D12: `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` + `DXGI_PRESENT_ALLOW_TEARING`
  - Vulkan: `VK_PRESENT_MODE_IMMEDIATE_EXT` under VRR display
- [ ] **R1.3 Frame pacing for VRR**:
  - Remove fixed 60 Hz vblank throttle when VRR active
  - Guest flip → immediate GPU present (no wait)
  - Let the display's VRR engine manage timing

### R2. FidelityFX Super Resolution (FSR)

- [ ] **R2.1 FSR 2.x integration**:
  - Bundle `ffx_fsr2_api_vk.dll` or link `FFX_FSR2` SDK
  - FSR 2 inputs: low-res color buffer, depth, motion vectors,
    exposure, reactive mask
  - Create FSR 2 compute pipeline from provided SPIR-V shaders
- [ ] **R2.2 FSR 3 frame generation**:
  - AMD Fluid Motion Frames (AFMF) for frame doubling
  - Optical flow interpolation between adjacent presented frames
  - Requires two-frame history buffer and motion vector input
- [ ] **R2.3 FSR quality presets**:
  - Config: `video.fsr = off|quality|balanced|performance|ultra_performance`
  - Quality → 1.5x scale, Balanced → 1.7x, Performance → 2.0x
  - Render resolution set by `resolution_scale` × FSR preset
- [ ] **R2.4 RCAS sharpening pass**:
  - Apply FSR's Robust Contrast Adaptive Sharpening as post-process
  - Configurable sharpness slider (0.0..1.0)
- [ ] **R2.5 FSR in render pipeline**:
  - Guest draw at low res → FSR 2 upscale → RCAS sharpen → present
  - Vulkan image layout transitions: COLOR_ATTACHMENT → COMPUTE_SHADER_READ
  - GPU timing queries for FSR pass duration in debug overlay

### R3. HDR Support (Long-term)

- [ ] **R3.1 Vulkan HDR swapchain**:
  - `VK_SURFACE_FORMAT_A2B10G10R10_UNORM_PACK32`
  - `VkSurfaceFullScreenExclusiveWin32InfoEXT` for exclusive fullscreen
- [ ] **R3.2 HDR metadata passthrough**:
  - Read ST.2086 mastering display metadata from game
  - Pass via `VkHdrMetadataEXT` to display
- [ ] **R3.3 Tonemapping (PQ → SDR)**:
  - ST.2084 (Perceptual Quantizer) to sRGB conversion shader
  - Auto-detect display HDR capability
  - Config: `video.hdr = auto|on|off`
