# EXTENDING PCSX5

Concrete, per-goal guides for the most requested enhancements. Each guide says
**exactly which component to touch** (and which to leave alone). Read
`COMPONENT_MAP.md` first if you are new to the layout.

Working rules that apply to every guide:
- Keep backends inside their own subdirectory; use the *L interface (`GpuDevice`,
  `AudioDevice`, `InputBackend`, `VideoDecoder`, `Platform::*`).
- Add every new option to the matching struct in `src/config/config.h`, and consume
  it in the subsystem that owns it (never in a backend you don't own).
- Build: `cmake --build build --config Release --target pcsx5_cli` (or the C# UI:
  `dotnet publish` per `build_release.ps1`). Tests: `ctest --test-dir build -C Release`.

---

## Guide A — Game modding (replace / inject game assets)

Goal: let the user replace or inject a game's files (e.g. a model, texture, or a
`data.js`) without breaking the emulator.

### Which component
- `src/kernel/` — `Kernel::TranslateGuestPath` (decl `src/kernel/kernel.h:54`,
  impl `src/kernel/kernel.cpp:286`) is the **single** guest-path→host-path
  translatior for `/app0/...` and `/savedata0/...`. Called by `src/kernel/syscalls.cpp`
  and the file HLE (`src/hle/libkernel.cpp`).
- `src/loader/` — the container/file layer that provides the raw game files.
- `src/compat/compat.{h,cpp}` — per-title override rules.

### Minimal approach (recommended first)
Make path lookup check a user mod-folder before the real game folder:
1. Add a `LoaderConfig.mod_dir` string in `src/config/config.h`.
2. In the one place that resolves a guest path to a host file (the path-translation
   function), if `mod_dir` is set and a file of the same relative path exists under
   it, return the mod path **first**.
3. This instantly gives "replace any file" modding for the whole tree with a single
   seam. Log the override (`LOG_INFO(Loader, "mod override %s <- %s", ...)`) so it is
   auditable.

### Advanced
- **Scripted/memory mods** (no file change): add a `CompatEntry` hook that is invoked
  after a module maps, and use `CpuCore`/HLE to patch guest memory (like KyTy's
  `gamePatch` byte patches — see `kyty_clone/src/loader/gamePatch.cpp` for the
  pattern: byte-signature match → `memcpy` replacement → flush icache).
- **Resource introspection for mods**: expose guest framebuffers / loaded textures to
  the frontend via `src/ipc/` so a GUI can list and edit them.

### Do NOT touch
`src/gpu/` render path, `src/cpu/`, `src/memory/` internals — a file-level mod works
entirely in the loader/compat seam.

---

## Guide B — Graphics enhancement: resolution scaling / upscaling / native hardware

Goal: render the guest output at a higher (or user-chosen) resolution, add upscaling
(FSR) / sharpening, and/or switch to a native graphics backend.

### Which component
All of it is `src/gpu/` (the GAL). Nothing in `src/hle/` or `src/cpu/` changes.

### Key seams (already present / scaffolded)
- **Swapchain + present**: `src/gpu/vk_present.h/.cpp` — owns back-buffer resolution,
  the VkSwapchain, and the final blit. This is where to change the **output render
  resolution** and aspect handling.
- **Upscaling**: `src/gpu/fsr_upscale.h/.cpp` (`FsrUpscale`, `FsrConfig`, quality
  presets + RCAS) is scaffolded and waits for a real FidelityFX implementation. This
  is your FSR point.
- **Frame buffer / texture upload**: `src/gpu/vk_draw.h` (`VkDrawBuffer`, `VkDrawCall`)
  — where the translated GCN draws upload guest textures. Upgrade render-target
  formats / MSAA here.
- **Shader translation**: `src/gpu/shader/` (`gcn_translate.*`, `spirv_builder.*`).
  Change how RDNA2 shaders → SPIR-V (e.g. to support more ops, or HDR output shaders).

### Recommended first step (resolution scale)
1. Add `GraphicsConfig.resolution_scale` (float or enum) to `src/config/config.h`.
2. In `vk_present.cpp`, multiply the swapchain image extent by the scale; scale each
   `GalViewport`/`GalScissorRect` (the GAL already models 3-way scissor clipping).
3. Make sure the guest-visible framebuffer semantics stay intact — the emulator is
   presenting a guest framebuffer, so scaling must happen at present-time, not inside
   guest-facing HLE.
4. Unit-check with the existing `vk_present_smoke` test target.

### Adding a native backend (e.g. D3D12)
- Implement `GpuDevice` from `src/gpu/gal.h` in a new `src/gpu/d3d12/` directory.
- It must provide: swapchain, image/buffer resources, pipeline + draw, readback,
  plus the shader path (translate the same SPIR-V that Vulkan uses, or a D3D
  equivalent via `src/gpu/shader/`).
- Register it in the GAL factory (where `VulkanDevice`/`NullGpuDevice` are chosen) and
  add `GraphicsConfig.renderer` value.

### Do NOT touch
`src/cpu/`, `src/memory/`, `src/hle/libagc.cpp`'s PM4 walker unless you are changing
*what* a draw does — resolution/render enhancement is purely a `src/gpu/` concern.

---

## Guide C — Native hardware (input / audio / platform) backends

Goal: support a new input device, audio endpoint, or OS abstraction in a
self-contained backend.

### Input — IAL (`src/gpu/input/`)
1. Implement `InputBackend` (from `input_backend.h`) in your own `.cpp`.
   Yield `ControllerState` + `InputCaps`; keep it raw/stateless (no game logic).
2. Add a `CreateMyInputBackend()` factory + a case in `InputBackend::Create`
   (`input_backend.cpp:15`).
3. Add `InputConfig.backend` value; document it.
Reference: `dualsense_input_backend.cpp` shows full HID (buttons, sticks, triggers,
touchpad, gyro, lightbar, adaptive triggers, mic).

### Audio — AAL (`src/hle/audio/`)
1. Implement `AudioDevice` into `audio_device.h` in your own `.cpp`
   (Open/Close/IsOpen/GetCaps/Output/Reset/GetVolume/GetLatencyFrames).
2. Add `CreateMyAudioDevice()` + case in `AudioDevice::Create`
   (`audio_device.cpp:32`). Keep `Output()` thread-safe (see `StereoRingBuffer`).
3. Add `AudioConfig.backend` value.
Reference: `wasapi_device.cpp` / `xa2_device.cpp` / `sdl_audio_device.cpp`.

### Platform — PAL (`src/common/platform/`)
Add a function here only if it wraps an OS service used by many subsystems
(memory protect, threads, fault handler, cpu features). Do not grow PAL for one-off
logic.

### Do NOT touch
`src/hle/libkernel.cpp` / `libaudioout.cpp` / `libpad.cpp` for a *backend* — those
already delegate to the *L factory. Only touch them if a **guest-facing ABI**
(return semantics / struct layouts) is wrong, which is separate from a backend.

---

## Guide D — Add a new config option the "right way"

1. Add the field to the matching section struct in `src/config/config.h`
   (e.g. `GraphicsConfig.preserve_aspect`, `AudioConfig.buffer_ms`).
2. Wire it in the subsystem that owns it (see COMPONENT_MAP §12).
3. If it affects the frontend, read it via the existing config callback / IPC.
4. Provide a sane default; keep `config.ini` and per-title overrides working
   (`src/compat/compat.*` already support per-title config).

---

## Guide E — Add a guest CPU instruction trace / step-tracer (debugging seam)

This is the tool that will unblock guest-native bug analysis (e.g. a game's own
native code doing something wrong), which is currently hard to observe.

Which component: `src/cpu/` + `src/memory/` (watchpoints) + `src/kernel/` (VEH).

Approach:
1. Reuse the VEH in `src/kernel/kernel.cpp` (it already parses the faulting
   instruction for TLS). Add an env-gated mode that, **before** resuming execution on
   an access violation, logs: `RIP`, the fault type (read vs write via
   `ExceptionInformation[0]`), and target address.
2. Add **guest write-watchpoints**: protect a guest page read-only and log the faulting
   RIP (PCSX5 already has `Memory::TrackGuestWrites` — extend its VEH hook to report
   RIP + write bytes once, then re-arm).
3. Optionally add a lightweight x86-64 **single-step** loop for a short range: patch the
   next guest instruction to `INT3`, let the VEH log the state, restore, advance
   (costly — keep it auto-disabled).

Do NOT touch HLE/GPU for this; it is purely a CPU/memory/kernel debug seam.

### Already implemented: GUEST NULL-CALL attribution
`src/kernel/kernel.cpp` (VectoredExceptionHandler) now detects a guest **null-call** —
an indirect call through a null/bad function pointer that faults by *executing* at
`RIP < 0x10000` (AV, `ExceptionInformation[0] == 8`) — and logs a
`GUEST NULL-CALL` banner with the guest registers plus a walk of the **current guest
thread's dedicated stack** (from `CpuCore::GetThreadById`) for guest return addresses,
so the guest call-site that made the null call is identified. The banner also dumps
the **full register set (RSI/RDI/R8–R15)** and a **guest-data window around RBX**
(usually the table/object the null pointer came from) — that data window is what
pins the exact slot feeding a `call rdi` with RDI=0. This is why ASTRO BOT's `RIP 0`
crash can be attributed without a full step-tracer. To use: boot the title; the banner
appears in the log at the crash (`[rsp...]` = return chain, `[caller ...]` = call-site
bytes, `[datarbx ...]` = RBX data object). Extend it (more register/target info) in
the same block.

### Already implemented: module-keyed dynamic TLS (DTV)
`src/kernel/kernel.cpp` `Kernel::ResolveDynamicTls(tid, ti_module, ti_offset, tp)` and
`src/hle/libkernel.cpp` `__tls_get_addr` (`vNe1w4diLCs#T#T`) implement variant-II
**module-keyed** TLS. Each `RegisterLoadedModule`'d module with a `PT_TLS` segment gets
a 1-based TLS-module index (`g_loaded_module_tls_index`, main module first). The main
module (and any unknown/malformed index) uses the original `thread_pointer + ti_offset`
shared-block path — **byte-identical** to the pre-DTV behavior, so single-module titles
(Dreaming Sarah) are unaffected. A KNOWN secondary TLS-bearing module (`ti_module > 1`)
gets a dedicated per-(thread,module) guest block lazily allocated and seeded from that
module's `PT_TLS` template (file-offset→segment RVA lookup via
`LoadedModule::segments`), mirroring KyTy `RuntimeLinker::TlsGetAddr` and SharpEmu
`GuestTlsTemplate`. Falls back to `tp + ti_offset` when the module is not found.
Validation note: the multi-module path is only exercised by titles that call
`__tls_get_addr` with `ti_module > 1`; neither current target does (ASTRO BOT calls it
only with module=0; Dreaming Sarah module 1), so it is additive infrastructure with a
provable no-op fast path. Seed/verify with a future multi-module TLS title before
relying on it.


---

## Working with tests

- After a backend/factory change, run the corresponding smoke target:
  `vk_present_smoke`, `hle_audio_pad_tests`, `input_*` if present.
- For pure CPU/kernel changes: `ctest --test-dir build -C Release --output-on-failure`.
- Golden image (`golden_capture`) covers PM4 render regressions.
