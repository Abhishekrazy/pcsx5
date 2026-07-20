# Pending — path to first pixels (Phase 5 M3 remaining)

Ordered, each independently verifiable.

## M3.2b — runtime SGPR model in translator (done)

- [x] `GcnTranslateOptions.initial_scalar_buffer_index`: when >= 0, load
      consumed SGPRs from that storage-buffer binding at shader start
      (port of SharpEmu's per-draw initial-scalar buffer) instead of baking
      constants — required so one shader serves many textures
      (sprite batching).
- [x] Port `ComputeConsumedScalarMask` (256-bit mask; 106/124/126 always set).
- [x] Draw path packs `evaluation.initial_scalar_registers` into an extra
      buffer binding per draw.  The draw path evaluates scalar state per
      draw and packs via `GcnPackInitialScalarState`; the executor uploads
      both 256-dword slots per draw and binds them at the trailing indices
      of the binding-0 StorageBuffer array (PS slot, then VS slot).

## M3.2c — guest_gpu draw executor

- [x] Guest image model: `guest address -> VkImage + view + render pass +
      framebuffer`, from `CB_COLOR0_BASE`/attrib in the Cx shadow
      (`src/gpu/vk_draw.cpp`; images seeded from guest memory at creation
      so the M1 CPU-side DMA clear is visible).
- [x] Texture: decode the evaluated image descriptor (dims, format),
      upload guest texels to a VkImage, bind as combined sampler.
      (Linear tiling assumed — verify on screen; tiled/BC formats and
      mipmapping deferred.)
- [x] Pipeline: from shadow state — topology (`VgtPrimitiveType` 0x242),
      blend (`CbBlend0Control`), raster (`PaSuScModeCntl`), viewport/scissor;
      no depth for the 2D path. Cached by (spirv digests + state).
- [x] Descriptors: storage buffers for evaluated buffer bindings (per-draw
      host-visible uploads of guest ranges) + the scalar-state buffer.
- [x] Index buffer: snapshot `index_count * (index_size?4:2)` bytes at
      `index_addr`, `vkCmdDrawIndexed`.

Deferred to M3.2d/M3.3: targetless composite draws (skipped until the flip
resolves the destination), present from the GPU image (draws currently land
in offscreen guest images only), command-batch submission (one submit per
draw today), window/generic/vport scissor intersection (only the screen
scissor is honored), storage images, detiling.

## M3.2d — present from GPU image

- [x] Flip: render-target image is the source of truth; present blits from
      it instead of the CPU guest-FB upload when a GPU image exists for the
      flipped buffer.  (`GPU::VkDrawLookupRenderTarget` resolves the guest
      address; `GPU::VkPresentFromImage` does the
      COLOR_ATTACHMENT -> TRANSFER_SRC blit into the swapchain image and
      restores the layout.  Draws and presents share the graphics queue, so
      the blit is ordered after pending draws; no image -> CPU-upload path
      falls back unchanged.)

## M3.3 — first real pixels

- [ ] Verify Dreaming Sarah draws land (menus on screen); fix format/
      tiling/blend mismatches against the golden SharpEmu dumps.

## Phase 6 (after pixels)

- [x] Audio: libSceAudioOut HLE (`src/hle/libaudioout.cpp`) — Init/Open/Close/
      Output/SetVolume/GetPortState/GetInfo, winmm waveOut backend, paced
      silence when `audio.backend=0`. (WASAPI/XAudio2 backends still TODO.)
- [x] Pad core: real 0x78 `ScePadData`, `scePadRead`/`GetHandle`/`OpenExt`,
      retail error codes, GLFW keyboard + XInput -> `GPU::GetCurrentPadState()`.
- [x] Pad completeness (rumble, touchpad, motion): `scePadSetVibration`
      routes ScePadVibrationParam to XInputSetState (real rumble); touchpad
      click via T key / XInput BACK (touch finger data neutral `touchNum=0`);
      `scePadGetMotionSensorData`/`scePadGetSensorData` return neutral gyro
      (zero) + 1g gravity on -Y. Real finger positions and motion data need
      the DualSense HID path (still TODO).

## Hygiene (any time)

- [x] Hunt the first-chance memcpy AV noise in SEH-guarded HLE paths.
      Sources found: HLE memcpy/memmove/memset handlers and AGC DMA
      fill/copy memcpy-ing unchecked guest pointers (now probed with
      `Memory::IsReadable/IsWritable` first), `sceSaveData*` out-pointer
      writes (now probed), and the kernel VEH itself: it ran the full
      crash-scan for benign first-chance exceptions (OutputDebugString
      0x40010006) and its own `SafeRead` stack probes ran off the thread
      stack top, raising fresh first-chance memcpy AVs recursively.  VEH
      now passes debug-output exceptions through silently and caps stack
      scans at `GetCurrentThreadStackLimits`.  PPSA02929 boot: VCRUNTIME
      memcpy AVs 52 -> 0, handler crashes 1 -> 0, crash scans 74 -> 0.
- [x] Fix `sceSysmoduleIsLoaded` AV (dispatcher-guarded today).
      Root cause: the handler's per-call LOG_INFO string formatting ran on
      the guest stack (dispatcher never switches to a host stack) and
      faulted on small/nearly-exhausted guest stacks.  Now logs at DEBUG
      (no formatting on the hot path) and returns SharpEmu-accurate codes:
      0 loaded, 0x80020002 not-loaded/unknown (was the always-0 "safe lie").
- [x] BUILDING.md test list is stale (says 18 tests; actual 30).
