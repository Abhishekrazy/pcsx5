# Pending — prioritized work queue

Low/medium priority first; high-priority items broken into small,
independently verifiable pieces.

Legend: `[ ]` pending · `[~]` in progress · `[x]` done

---

## Phase H — H4 Menu Blocker Resolution (High Priority)

- [x] H4.1 Construct runtime null variants — HeapAllocLocked zeroes allocations, Value ctors write type
- [x] H4.2 VCRUNTIME140 AV — IsValidGuestPointer + AGC paths validated + SEH guards
- [x] H4.3 VEH recursion guard (max depth 8) + heartbeat diagnostics
- [x] H4.4 Golden frame infrastructure: PM4 capture/replay + readback + PNG compare exists in `tools/pm4_replay.cpp` and `vk_present.cpp`.  Actual golden capture requires running a game title to produce reference frames.

---

## Phase A — Hardware Abstraction Layers

### A1. Graphics Abstraction Layer (GAL)
- [x] A1.1 Define `GpuDevice` interface (`src/gpu/gal.h`) with Null backend + factory
- [x] A1.5 Runtime backend selection (Vulkan / GDI / Null with auto-probe)
- [ ] **A1.2 Refactor Vulkan backend into `VulkanDevice : GpuDevice`** — wire vk_context/vk_present/vk_draw through the interface
- [ ] **A1.3 GDI/Software fallback** as `GdiDevice : GpuDevice`
- [ ] **A1.4 D3D12 backend** (stretch)

### A2. Audio Abstraction Layer (AAL)
- [x] A2.1 Define `AudioDevice` interface (`src/hle/audio/audio_device.h`)
- [x] A2.2-A2.4 Concrete backends: WaveOutDevice, WasapiDevice, Xa2Device
- [x] A2.7 Backend factory + PacingAudioDevice null backend
- [x] A2.5 SDL Audio backend (`SdlAudioDevice : AudioDevice`, dynamically loads SDL2.dll)

### A3. Input Abstraction Layer (IAL)
- [x] A3.1 Define `InputBackend` interface (`src/gpu/input/input_backend.h`)
- [x] A3.2-A3.3 GlfwKeyboardBackend + XInputBackend
- [x] A3.5-A3.6 DualSenseInputBackend + InputMultiplexer
- [ ] **A3.4 Add SDL GameController** (`SdlGameController : InputBackend`)

### A4. Platform Abstraction Layer (PAL)
- [x] A4.1-A4.6 Define `Platform::*` API + full Windows implementation

---

## Phase D — DualSense HID Fixes & Improvements

- [x] D1 BT output report format (report[2] 0x10→0x00) + mic audio extraction from input reports
- [x] D2 Lightbar RGB — fixed enable flag bits and byte positions per SDL spec
- [x] D3 ButtonLayout shared component (ImGui renderer + battery status + "tested" checklist)
- [x] D4 Player LEDs wired (scePadSetPlayerIndicator → SetPlayerLeds) + battery in overlay + scePadSetLightBar → SetLightBar
- [x] D3.3 WPF ControllerVisualizer UserControl (animated buttons, sticks, triggers, touchpad, battery)
- [ ] **D3.4 Replace standalone tester tools** (redirect to unified `--test-input` CLI)

---

## Phase R — Rendering Enhancements

### R1. VRR
- [x] R1.1 Vulkan swapchain present mode selection (FIFO_RELAXED / IMMEDIATE for VRR)
- [x] R1.2 Config-driven (`video.vrr`) via `VkPresentSetVrrConfig()`
- [ ] **R1.3 Frame pacing for VRR** (remove fixed 60 Hz vblank throttle)

### R2. FidelityFX Super Resolution
- [x] R2.1 FsrUpscale class with DLL detection + quality presets + resolution helpers
- [ ] **R2.1-R2.5 Full FSR integration** (needs AMD FidelityFX SDK headers for Fsr2CreateContext/Dispatch)

### R3. HDR
- [ ] R3.1-R3.3 HDR swapchain, metadata passthrough, PQ→SDR tonemapping

---

## Phase P — PS5 PKG Extraction & ELF Loading

### P1-P2 PS5 PKG + PFS
- [x] P1.1 PS5 PKG parser (magic 0x7F464948, entry table, PFS layout)
- [x] P1.2 fPKG AES-128-CBC entry decryption
- [x] P1.3 Auto-detect PS4 vs PS5 PKG by magic in CLI `--extract-pkg`
- [x] P2.2 `ExtractEbootFromPkgPs5`: PKG→PFS→mount→eboot.bin end-to-end
- [ ] **P2.1 PFS reader: compressed block support**
- [x] P3 SELF→ELF extraction (already implemented in elf.cpp: LoadSelf + ExtractInnerElf, works for fPKG; rejects encrypted retail SELFs)

### P4. PkgToolBox
- [x] P4.1 Audit PkgToolBox PS5 logic — entry table format matches ({u32 id, u32 type, u64 offset, u64 size, u64 pad}).  Layout table at 0x400 matches (8 × u64).  Minor: Python reads u64 for file count, C++ reads u32 — acceptable.
- [x] P4.4 Document extraction workflow (`wiki/pkg-extraction.md`)

---

## Phase V — Video Decoder Support

### V1. VDAL Interface
- [x] V1.1 `VideoDecoder` interface (open/seek/decode/audio tracks)
- [x] V1.2 `VideoFrame` structure (RGBA8/YUV420/NV12 + timing)
- [x] V1.3 Backend factory with Bink2/CRI/FFmpeg/Null + format auto-detection by magic bytes

### V2-V5 Full Decoder Implementation
- [ ] **V2.1 Bink2Decoder** (needs Bink SDK headers for bink2w64.dll exports)
- [ ] **V3.1 CriUsmDecoder** (needs FFmpeg or H.264/H.265 decoder backend)
- [ ] **V4.1 FFmpegDecoder** (needs FFmpeg headers + avformat/avcodec DLL linking)
- [ ] V5.1-V5.3 GPU texture upload, overlay compositing, A/V sync

---

## Phase O — Performance Optimization

### O1. Memory
- [x] O1.1 Large page support (MEM_LARGE_PAGES for ≥2 MB allocations)
- [ ] **O1.2 Direct-mapped guest memory** (single large VirtualAlloc reservation)
- [x] O1.3 Pre-commit 256 MB at 0x800000000 on boot
- [x] O1.4 Write-tracking coalescing (merge adjacent ranges → fewer VirtualProtect calls)

### O2. GPU Pipeline
- [x] O2.1 Command batching (256 draws/batch, submitted once per flip)
- [x] O2.2 Persistent VkPipelineCache (disk cache between runs)
- [ ] **O2.3 Descriptor pool pre-allocation + reuse**
- [ ] **O2.5 Shader cache format optimization** (versioned, incremental)

### O3. CPU/Threading
- [x] O3.3 VEH TLS cache (thread_local t_tls_base, no mutex contention)
- [x] O3.1 Thread affinity + naming (affinity_mask config + SetThreadName)
- [ ] **O3.2 Guest thread scheduling** (reduce mutex contention in sync primitives)

### O4. Audio Buffers
- [ ] **O4.1 Shared ring buffer across backends** (reduce lock contention)
- [ ] **O4.2 Zero-copy guest audio read**

### O5. Boot Time
- [ ] **O5.1 Parallel module loading** (thread pool for PRX relocation)
- [ ] **O5.2 Shader warmup from disk cache** (background compilation)

---

## Phase S — System & Quality

### S1. Build
- [ ] **S1.1 Split CMakeLists per subdirectory** (faster incremental builds)
- [ ] **S1.2 Clang-CL build** (validate with both MSVC and Clang)
- [ ] **S1.3 Headless CI test runs** (GPU-less)
- [ ] **S1.4 PKG extraction CI test**

### S2. Testing
- [x] S2.1 PS5 PKG parser unit test (synthetic valid PKG, all assertions pass)
- [x] S2.2 Frame timing waterfall + FPS overlay (256-entry ring + ImGui stacked bars)
- [ ] **S2.3 Crash bundle: include config + trace ring + pipeline cache**

### S3. Documentation
- [x] S3.1 Architecture docs: README + PROGRESS updated with all abstraction layers
- [ ] **S3.2 Developer guide**: how to add backends, platform porting guide, testing guide
