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

### S3. Documentation

- [ ] **S3.1 Architecture docs update**:
  - Document new abstraction layers (GAL/AAL/IAL/PAL)
  - Add PKG extraction workflow docs
  - Update wiki with PS5 PKG format notes
- [ ] **S3.2 Developer guide**:
  - How to add a new backend
  - How to add abstraction for a new platform
  - Testing guide for hardware backends
