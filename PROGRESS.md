# pcsx5 progress (as of 2026-07-24)

## Phases 0–4 — complete

Kernel (real pthread/equeue/sema/event-flag sync, demand-commit memory,
unified clock), broad HLE (savedata, user/system services, NP, sysmodule,
libc heap + printf, videoout flip model), 553+ NID-DB stubs, per-title
config, `/app0`/`/savedata0` filesystem translation. 30/30 ctest green.

## Phase 5 (GPU) — milestone status

| Milestone | Status | Proof |
| --------- | ------ | ----- |
| M0 AGC HLE | done | `sceAgcCreateShader` real ABI, register-default tables, 20+ PM4 builders, submit walker with Cx/Sh/Uc shadow state (`src/hle/libagc.cpp`) |
| M1 Vulkan backend | done | Dynamic loading (no SDK), swapchain present proven pixel-correct, GDI fallback (`src/gpu/vk_*`) |
| M1.5 boot blockers | done | Both games boot crash-free (see below) |
| M2.1 shader foundation | done | Full RDNA2 decoder + metadata parser + SPIR-V builder (`src/gpu/shader/`); corpus of 81 real shaders decodes |
| M2.2 SPIR-V emission | done, `c923dd6` | **81/81 corpus shaders translate; 81/81 accepted by NVIDIA ICD** |
| M3.0 draw-time translation | done, `be2571a` | In-game: VS+PS found via register shadow at draw, translated, driver-validated |
| M3.1 scalar evaluator | done, `2e165a7` | In-game: per-draw texture/vertex/constant buffer descriptors resolve |
| M3.2a vertex-format decode | done | Dynamic descriptor-driven `BufferLoadFormat*` decode in translator |
| **M3.3 menus (H4)** | 🟡 blockers in progress | VEH recursion guard added, AGC pointer validation added, heap zeroing confirmed (`HeapAllocLocked` zeros allocations) |
| **R1 VRR** | 🔵 new | `VK_PRESENT_MODE_FIFO_RELAXED_KHR` + IMMEDIATE modes, VRR config |
| **R2 FSR** | 🔵 scaffolded | `FsrUpscale` class, DLL detection, quality presets, needs SDK |

## Game status

- **Dreaming Sarah (PPSA02929)**: boots indefinitely crash-free, submits
  draws + flips every frame, Vulkan presents the guest FB.
- **LOST EPIC (PPSA07429)**: boots through IL2CPP assembly loading.

## Phase 6 — Audio & Input Abstraction Layers (new, 2026-07-23)

### Audio (AAL)
- `AudioDevice` interface with 4 backends: WaveOut, WASAPI, XAudio2, Pacing
- Factory auto-probes: XAudio2 → WASAPI → waveOut → Pacing
- Volume scaling, stereo PCM16 conversion across all backends
- Config: `audio.backend = 0|1|2` (Off / WASAPI / XAudio2)

### Input (IAL)
- `InputBackend` interface with 4 backends: GLFW keyboard, XInput, DualSense HID, Null
- `InputMultiplexer` for merging multiple backends per frame (DualSense touch+motion always wins)
- Full DualSense HID support: buttons, sticks, triggers, touchpad (2 fingers), gyro/accel, rumble, adaptive triggers, battery, mic audio
- Config: `input.backend = keyboard|xinput|dualsense|null`
- Lightbar RGB and player LED wiring via scePadSetLightBar / scePadSetPlayerIndicator

### Bluetooth DualSense Fixes
- BT output report[2]: changed from 0x10→0x00 to fix mic/speaker enable
- Mic audio extraction: 4-channel × 8-sample PCM16 from input report offset o+74

## Phase 6b — Graphics Abstraction Layer (GAL, new, 2026-07-23)

- `GpuDevice` interface: swapchain, resources, pipelines, draw commands, readback
- Backends: VulkanDevice (adapter over existing vk_*), GDI (stub), Null (headless)
- Factory auto-probes: Vulkan → GDI → Null
- Runtime backend selection via `gpu.backend` config

## Phase 6c — Platform Abstraction Layer (PAL, new, 2026-07-23)

- `Platform::*` namespace: VirtualAlloc/mmap, threads, dynamic libs, VEH, CPU features
- Full Windows implementation with large page support
- CPU feature detection: SSE4a, BMI1/2, ABM (POPCNT/LZCNT), AVX/AVX2

## Phase 6d — Video Decoder Abstraction Layer (VDAL, new, 2026-07-23)

- `VideoDecoder` interface: open/seek/decode to RGBA8/YUV frames, A/V sync
- Format auto-detection by magic bytes: Bink2 (KB12), CRI USM, MP4, WebM
- Backends: Bink2 (bink2w64.dll), CRI USM, FFmpeg (avformat-61.dll)
- NullVideoDecoder fallback when no backend available

## Phase P — PS5 PKG Extraction (new, 2026-07-23)

- PS5 PKG parser (`src/loader/pkg_ps5.*`): magic 0x7F464948, entry table with
  file names, PFS image offset/size from layout table @0x400
- fPKG AES-128-CBC entry decryption with scene passcode
- `ExtractEbootFromPkgPs5`: PKG → PFS → mount → eboot.bin end-to-end pipeline
- `--extract-pkg` CLI auto-detects PS4 vs PS5 PKG by magic byte
- `ExtractAnyPkg` dispatcher: PS4 (0x7F434E54) → ParsePkg, PS5 → ExtractPkgPs5

## Phase O — Performance Optimization (new, 2026-07-24)

- **O1.1**: Large page support — `Map()` tries `MEM_LARGE_PAGES` (2 MB) for
  allocations >= 2 MB, falls back to 4 KB pages
- **O2.1**: kBatchDraws increased 64→256, all draws batched per frame
- **O3.3**: VEH TLS cache — thread-local `t_tls_base` eliminates global mutex
  on every VEH TLS trap

## Known non-blocking issues

- ~47 first-chance `VCRUNTIME140` memcpy AVs reading guest heap — caught by
  SEH guards, log noise.
- One `sceSysmoduleIsLoaded` AV caught by the dispatcher SEH guard.
- Translator rejects loudly (by design): DPP, VOP3P packed-f16, DS,
  global-memory, buffer store/atomic, storage image load/store, compressed
  export precision shadow, offset/compare image samples.
