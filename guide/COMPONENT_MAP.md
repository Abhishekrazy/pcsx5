# PCSX5 Component Map

The single authoritative reference for **what every subsystem and key class does**,
**who owns what**, and **which extension seam to touch** for a given feature.

Use this so you never modify code that is irrelevant to the task you are working on.
Each entry: **responsibility** · **ownership rule** · **extension seam** (✔ = plug here).

> This map is generated from the actual `src/` inventory (class names verified from the
> headers/.cpp files). If a class is not listed, it is a small local helper — check the
> file header comment first.

---

## 0. Component index (top-level)

| Component | Path | What it owns | Touched by |
|---|---|---|---|
| Core API | `src/core_api.h/.cpp` | Init/Load/Run/Stop/Shutdown seam for the UI host | Backends, frontends |
| Platform (PAL) | `src/common/platform/` | OS services (memory, threads, VEH, CPU features) | **All** |
| Loader | `src/loader/` | PKG/PFS/SELF/ELF parse, modules, relocation | Loading features |
| Kernel | `src/kernel/` | VEH, syscalls, fd table, threads | Syscall work |
| CPU | `src/cpu/` | Guest thread lifecycle, syscall adapters | Thread work |
| Memory | `src/memory/` | Guest address space, fault/VFH, write tracking | Memory work |
| HLE | `src/hle/` | PS5 userland library emulation | **Game compatibility** |
| GPU/GAL | `src/gpu/` | Graphics abstraction + Vulkan + shader translate | **Graphics enhancement** |
| Input (IAL) | `src/gpu/input/` | Input abstraction + backends | **Native input** |
| Audio (AAL) | `src/hle/audio/` | Audio abstraction + backends | **Native audio** |
| Video (VDAL) | `src/media/` | Video decoder abstraction + backends | Cutscenes |
| Compat | `src/compat/` | Per-title compatibility DB | Game-specific patches |
| Diagnostics | `src/diagnostics/` | Crash handler, frame timing | Debugging |
| Config | `src/config/` | JSON config schemas | Any new option |
| Reports | `src/reports/` | JSON compat reports | Testing |
| System | `src/system/` | Host OS/CPU/GPU info | Frontend |
| IPC | `src/ipc/` | Shared-memory frame sharing to UI | GUI frame display |
| UI | `src/ui/` (im), `src/ui_csharp/` (WPF) | Overlay / desktop frontend | Frontend work |

---

## 1. Platform Abstraction Layer (PAL) — `src/common/platform/platform.{h,cpp}`

- **Classes**: `Platform::MemoryProtection`, `MemoryType`, `CpuFeatures`, functions `VirtualAlloc/VirtualFree/VirtualProtect/VirtualQuery`, `EnableLargePages`, threads, `InstallFaultHandler`, CPU feature detection.
- **Responsibility**: every OS- (Windows) dependent call. No `#ifdef` leak outside.
- **Ownership rule**: **the only file you edit to port to a new OS or change memory strategy.**
- **Extension seam** ✔ **Native hardware**: add/alter a backend here to change how guest memory is backed (e.g. host pages, large pages).

---

## 2. Loader — `src/loader/`

- **ELF/SELF**: `elf.h` (`LoadedModule`, `MappedSegment`, `SelfImage`, `SelfSegment`, `TlsTemplate`).
- **Containers**: `pkg.h` (`PkgImage`), `pkg_ps5.h` (`PkgPs5Image`), `pfs.h` (`PfsImage`), `param_json.h` (`ParamJson`).
- **Module resolution**: `module_resolver.h` (`ModuleResolver`), `module_graph.h` (`ModuleGraph`).
- **Responsibility**: turn a real PS5 game's containers into mapped, linked guest modules; NID→symbol linking.
- **Ownership rule**: nothing under loader touches GAL/AAL/IAL; it only feeds `Kernel`/`HLE`.
- **Extension seam** ✔ **Game modding (file overrides)**: the module/param/PFS layer is where you intercept a game's files (e.g. replace `data.js`, models, textures) — see `EXTENDING.md`. ✔ **Self/ELF enhancements** are here.

---

## 3. Kernel — `src/kernel/`

- `kernel.h` (`ThreadContext`), `syscalls.cpp`, `fd_table.h` (`FdEntry`), `thread.cpp`, `tls.h` (`GuestTlsContext`), `tls_patch.h`, `instr_decode.h`, `memory.cpp` VEH.
- **Responsibility**: the `VectoredExceptionHandler` (VEH) that catches guest syscalls (`INT3`→syscall), TLS `fs:0` emulation, demand-commit faults, and guest crashes. Syscall dispatch table.
- **Ownership rule**: kernel interprets the CPU-fault-sourced events and dispatches to `CpuCore`/`HLE`; it must not know about specific games.
- **Extension seam** ✔ **New syscalls / byte-patching** live here. ✔ **Game-specific load patches** would hook here.

---

## 4. Cpu core — `src/cpu/cpu.{h,cpp}` (direct execution)

- `CPUState`, `GuestThread`, `XmmReg`; `CpuCore::CreateThread/JoinThread/InvokeSyscall`, `amd_compat.h` (`DecodedInstruction`).
- **Responsibility**: guest threads (1:1 host threads) and the SysV→host context bridge. Guest code runs **natively** (no JIT).
- **Ownership rule**: only thread lifecycle and syscall adapter. It does not decode/translate the guest bytecode.
- **Extension seam** ✔ **If you want a GPU/JIT or instruction trace / step-tracer**, this is the layer to add it (currently direct execution only).

---

## 5. Memory — `src/memory/memory.{h,cpp}`

- `Memory::*` (read/write helpers), `Status`, `MemoryInfo`, `MemoryStats`; `TrackGuestWrites`, demand-commit, VFH.
- **Responsibility**: guest address space identity-mapped to host; page protection; write tracking (used by GPU texture cache).
- **Ownership rule**: single owner of guest mappings; every other subsystem asks `Memory::` for guest memory.
- **Extension seam** ✔ **Guest-write watchpoints / trace** (needed to debug guest-native bugs) plug in here via the VEH + `TrackGuestWrites`.

---

## 6. HLE — `src/hle/`  ⚠️ LARGE — the game-compatibility layer

- **Dispatchers**: `hle.{h,cpp}` (`HleSymbol`, `GuestArgs`, `ImportStats`, `TraceEntry`), `dispatcher.asm`.
- **Libraries**: `libkernel.cpp`, `liblibc.cpp`, `libagc.cpp`, `libvideoout.cpp`, `libpad.cpp`, `libaudioout.cpp`, `libsavedata.cpp`, `libsysmodule.cpp`, `libsystemservice.cpp`, `libuser.cpp`, others under `src/hle/`.
- **Audio backends** in `src/hle/audio/` (`AudioDevice` = AAL interface; `WasapiDevice`, `Xa2Device`, `WaveOutDevice`, `SdlAudioDevice`, `PacingAudioDevice`).
- **Responsibility**: emulating the PS5's userland libraries (libc, libkernel, libSce*...). This is where **per-game correctness** lives.
- **Ownership rule**: each `lib*.cpp` owns exactly its module's symbols. `hle.cpp` owns symbol registration/dispatch. **Do not put graphics/input/audio behavior here** — delegate to the *L layers.
- **Extension seam** ✔ **Game compatibility fixes** (making a game boot/advance) are almost always here. ✔ **Modding APIs** (exposing game resources) originate here.

### HLE calling-convention contract (for writing stubs)
`GuestArgs`: arg1..arg6 = `rdi rsi rdx rcx r8 r9`, `stack_args` = 8th+. XMM returns via `GetIncomingXmm0()`. Pointers are identity-mapped (cast to host pointer). Register with `HLE::RegisterSymbol("libX", "<nid-or-name>", lambda)`; auto-stub catches the rest.

---

## 7. GPU / Graphics Abstraction Layer (GAL) — `src/gpu/`  ⚠️ for graphic enhancement

- **Interface**: `gal.h` (`GpuDevice`, `GalCaps`, `GalImageDesc`, backends).
- **Backends**: `src/gpu/vulkan/vulkan_device.cpp` (`VulkanDevice`), `NullGpuDevice`.
- **Present/draw**: `vk_present.h`, `vk_draw.h`, `vk_context.h`, `fsr_upscale.h` (`FsrUpscale`), `shader_cache.h`.
- **Shader pipeline**: `src/gpu/shader/` — GCN decode (`gcn_decode.h`), translate (`gcn_translate.h`, `spirv_builder.h`), eval (`gcn_eval.h`), metadata (`metadata.h`).
- **Responsibility**: render the guest framebuffer to a host window; translate RDNA2/GCN shaders to SPIR-V; present.
- **Ownership rule**: guest code reaches GPU **only** through GAL (`GpuDevice`). Backend headers (`vk_*.h`) are never included outside their impl. The RDNA2 metadata decoding is done (M2.x).
- **Extension seam** ⭐ **Resolution scaling / upscaling / render enhancement**:
  - Change/grow the swapchain + present path in `vk_present.cpp`.
  - Extend `FsrUpscale` (already scaffolded) for FSR/RCAS.
  - Change how shaders/descriptors are built in `src/gpu/shader/`.
  These are **isolated to `src/gpu/`**; you should rarely touch HLE.

---

## 8. Input Abstraction Layer (IAL) — `src/gpu/input/`

- **Interface**: `input_backend.h` (`InputBackend`, `ControllerState`, `InputMultiplexer`, `InputCaps`).
- **Backends**: `glfw_keyboard_backend.cpp` (`GlfwKeyboardBackend`), `xinput_backend.cpp` (`XInputBackend`), `dualsense_input_backend.cpp` (`DualSenseInputBackend`), `sdl_gamecontroller_backend.cpp`, `input_bot.{h,cpp}` (replay).
- **Responsibility**: merge controller/keyboard/touch/motion into one `ControllerState` per frame.
- **Ownership rule**: inputs are merged by `InputMultiplexer`; each backend only reports raw state.
- **Extension seam** ✔ **Native controller / new input source**: implement `InputBackend` in a new `.cpp`, add a `Create*Backend`, register in `InputBackend::Create` (`input_backend.cpp:15`), add a config option.

---

## 9. Audio Abstraction Layer (AAL) — `src/hle/audio/audio_device.{h,cpp}`

- **Interface**: `AudioDevice`, `AalFormat`, `AalCaps`, `AalBackendType`.
- **Backends**: `WaveOutDevice`, `WasapiDevice`, `Xa2Device`, `SdlAudioDevice`, `PacingAudioDevice`; `ring_buffer.h` (`StereoRingBuffer`).
- **Responsibility**: deliver guest audio PCM to a host endpoint; volume/latency handling.
- **Ownership rule**: `AudioDevice::Create(type)` is the single factory (`audio_device.cpp:32`).
- **Extension seam** ✔ **Native audio / new endpoint**: add `Create<Name>Device()` + case in the factory + config option. Keep `Output()` thread-safe.

---

## 10. Video Decoder Abstraction Layer (VDAL) — `src/media/video_decoder.{h,cpp}`

- **Interface**: `VideoDecoder`, `VideoFrame`, `VideoDecoderConfig`; backends `FFmpegVideoDecoder`, Bink2/CRI wrappers, `NullVideoDecoder`.
- **Responsibility**: decode in-game cutscenes to RGBA/YUV frames.
- **Ownership rule**: `VideoDecoder::Create` factory; formats auto-detected by magic bytes.
- **Extension seam** ✔ **New codec/container** plugs in here.

---

## 11. Compat, Diagnostics, Config, Reports, System, IPC, UI

- `src/compat/compat.{h,cpp}` (`Entry`, `Status`) — per-title override/history JSON. **Game-patch seam** ✔.
- `src/diagnostics/` (`CrashContext`, `frame_timing.h`) — crash bundles, frame timing.
- `src/config/config.h` — `Config` + per-section structs (`GraphicsConfig`, `AudioConfig`, `InputConfig`, ...). **Every new option registers here.**
- `src/reports/reports.h` — compat/regression JSON reports.
- `src/system/system.h` — host info (`CpuInfo`, `GpuInfo`, `OsInfo`).
- `src/ipc/ipc_shared.{h,cpp}` (`IpcShared`) — shared-memory frame handoff to the UI.
- `src/ui/button_layout.h`, `src/ui_csharp/` (WPF) — HUD overlay and desktop GUI frontend.

---

## 12. Quick "which file do I edit for X?" table

| If you want to... | Edit these (and only these) |
|---|---|
| Make a specific game boot/advance | `src/hle/lib*.cpp` (the module it stalls in) + `src/compat/compat.*` |
| Add game modding (replace a game file) | `src/loader/` (file/mount layer) + `src/compat/` |
| Change resolution / upscaling / render output | `src/gpu/` (GAL `vk_present.*`, `fsr_upscale.*`, `src/gpu/shader/`) |
| Add a native GPU backend (D3D12/etc.) | `src/gpu/gal.h` + new `src/gpu/` backend dir; register in the GAL factory |
| Add a new input source / controller | `src/gpu/input/` (implement `InputBackend` + factory) |
| Add a new audio endpoint | `src/hle/audio/` (implement `AudioDevice` + factory) |
| Add a new video codec | `src/media/` (implement `VideoDecoder` + factory) |
| Change memory / system allocation strategy | `src/common/platform/platform.*` (PAL) + `src/memory/` |
| Add a new config option | `src/config/config.h` (+ the subsystem that consumes it) |
| Add syscall / byte-patch support | `src/kernel/` |
| Add a guest instruction trace / step-tracer | `src/cpu/` + `src/memory/` (watchpoint/VEH) |

---

## 13. Golden rules (to avoid touching the wrong thing)

1. **Guest code touches GPU only via GAL**, audio only via AAL**, input only via IAL**, video only via VDAL**, OS only via PAL**. If a fix starts by editing a `www_backend` outside its own directory, stop and reconsider.
2. **Backend headers never leak out of their implementation dir.** If your change needs `vk_*.h` outside `src/gpu/vulkan/`, you've broken the seam — use the `GpuDevice` interface instead.
3. **Per-game fixes belong in `src/hle/` or `src/compat/`**, never in `src/gpu/`/`src/cpu/`/`src/memory/` unless the game exposes a genuine emulator-core bug.
4. **Every new backend/option touches exactly two spots**: the interface impl (its own dir) and the factory (`Create`) + a config struct.
5. **Re-verify header include boundaries** before committing; `/W4 /WX` enforces many of these.
