# Pending — path to first pixels (Phase 5 M3 remaining)

Ordered, each independently verifiable.

## M3.2b — runtime SGPR model in translator (in progress)

- [ ] `GcnTranslateOptions.initial_scalar_buffer_index`: when >= 0, load
      consumed SGPRs from that storage-buffer binding at shader start
      (port of SharpEmu's per-draw initial-scalar buffer) instead of baking
      constants — required so one shader serves many textures
      (sprite batching).
- [ ] Port `ComputeConsumedScalarMask` (256-bit mask; 106/124/126 always set).
- [ ] Draw path packs `evaluation.initial_scalar_registers` into an extra
      buffer binding per draw.

## M3.2c — guest_gpu draw executor

- [ ] Guest image model: `guest address -> VkImage + view + render pass +
      framebuffer`, from `CB_COLOR0_BASE`/attrib in the Cx shadow.
- [ ] Texture: decode the evaluated image descriptor (dims, format),
      upload guest texels to a VkImage, bind as combined sampler.
      (Assume linear tiling; verify on screen.)
- [ ] Pipeline: from shadow state — topology (`VgtPrimitiveType` 0x242),
      blend (`CbBlend0Control`), raster (`PaSuScModeCntl`), viewport/scissor;
      no depth for the 2D path. Cache by (spirv digests + state).
- [ ] Descriptors: storage buffers for evaluated buffer bindings (per-draw
      host-visible uploads of guest ranges) + the scalar-state buffer.
- [ ] Index buffer: snapshot `index_count * (index_size?4:2)` bytes at
      `index_addr`, `vkCmdDrawIndexed`.

## M3.2d — present from GPU image

- [ ] Flip: render-target image is the source of truth; present blits from
      it instead of the CPU guest-FB upload when a GPU image exists for the
      flipped buffer.

## M3.3 — first real pixels

- [ ] Verify Dreaming Sarah draws land (menus on screen); fix format/
      tiling/blend mismatches against the golden SharpEmu dumps.

## Phase 6 (after pixels)

- [ ] Audio: libSceAudioOut ring buffers -> XAudio2 (`pcsx5_savedata`-style
      per-title config already has audio section).
- [ ] Pad completeness (rumble, touchpad, motion).

## Hygiene (any time)

- [ ] Hunt the first-chance memcpy AV noise in SEH-guarded HLE paths.
- [ ] Fix `sceSysmoduleIsLoaded` AV (dispatcher-guarded today).
- [ ] BUILDING.md test list is stale (says 18 tests; actual 30).
