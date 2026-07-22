# Pending — prioritized work queue

Low/medium priority first; high-priority items broken into small,
independently verifiable pieces. Completed Phase 5 M3 details moved to
PROGRESS.md / ROADMAP.md.

## High priority — M3.3 menus (broken into pieces)

Context: splash renders correctly; after ~180 frames the guest enters a
content-load phase (no draws), then the run dies silently ~8-10 min in.

- [x] H1. Fix the silent death: reproduce with full logging, catch where
      DIAGNOSED and instrumented (heartbeat, CRT hooks, stack guarantee).
      Fixed by H3 (C++ exception unwind support) — the game's catch handler
      now runs correctly for exception types it can handle.
- [~] H4. Verify menus render after H1-H3; compare against SharpEmu
      golden dumps; fix remaining format/tiling/blend mismatches.
      IN PROGRESS (2026-07-22): two independent crashes identified.
      
      Crash A — Construct runtime null variants (tag=0xb0):
        Applied fix: zero user data in HeapAllocLocked (liblibc.cpp:97-100,
        109-111) to match Orbis OS zero-initialized heap expectations.
        Hypothesis: Construct runtime's internal allocator reuses freed
        blocks without clearing; stale 0xb0 poison bytes from freed blocks
        appear as null variants during JSON parsing.  Not yet verified.
      
      Crash B — VCRUNTIME140.dll access violation (0xC0000005 at +0x1cc54):
        Occurs during AGC texture ops (d-6uF9sZDIU + memcpy chain).  VEH
        catches it (logs + CONTINUE_SEARCH), game survives hundreds more
        frames.  The guest memory scan finds leaked host pointers (0x7ff...)
        in guest memory — likely root cause.  Eventually one crash becomes
        a C++ exception with no handler → std::terminate.
        Not yet diagnosed: which HLE function writes host pointers into
        guest memory.  Requires targeted logging of AGC map/write paths.
      
      Both blockers remain open.  Diagnostics in src/hle/liblibc.cpp:
      throw-type/message dump, failing-variant tag dump, parser-frame
      and array-element dumps, .work/guest_text.bin snapshot.

## High priority — later GPU work (after menus)

- [~] H6. Compute queue support.
      Outcome 2026-07-20 (wave 2): packet plumbing is complete -
      `sceAgcCbDispatch`/`sceAgcDcb/AcbDispatchIndirect` emit
      IT_DISPATCH_DIRECT/INDIRECT packets, the submit walker parses and
      counts them, and COMPUTE_PGM_LO/HI (kComputePgmLo/Hi) is tracked in
      the Sh shadow.  Log sweep of every capture in .work/*.log shows
      zero dispatches from the 2D titles, so the executor is DEFERRED:
      dispatch packets now log a clear WARN ("NOT IMPLEMENTED ... H6;
      dispatch dropped", includes group dims + CS address) instead of a
      silent INFO.

      ## H6 implementation plan

      ### Phase 1 - compute shader translation (2026-07-22)
      - [x] 1.1 Add `GcnSpirvStage::Compute` to gcn_translate.h:
        Entry-point with GLCompute execution model, gl_GlobalInvocationID,
        gl_WorkGroupID, gl_LocalInvocationID builtin inputs.
        Workgroup size from LocalSize(workgroup_size_x/y/z).
      - [x] 1.2 Reject unsupported compute patterns loudly (DS_*,
        atomics) at TryCompile entry.
      - [x] 1.3 Extend GcnTranslateOptions: workgroup_size_x/y/z on the
        options struct; GcnTranslateDefaultOptions defaults to (64,1,1).
      - [x] 1.4 GcnTranslate compute stage routing: DeclareModule adds
        compute cap, DeclareStageInterface declares builtin Input vars,
        EmitInitialState loads WorkgroupId->SGPR[0..2],
        LocalInvocationId->VGPR[0..2].  TryEmitExport returns true (no-op).
      - [x] 1.5 CachedAttributeCount returns 0 for compute (no interface
        attributes).

      ### Phase 2 - resource binding for compute
      - [ ] 2.1 DescriptorSetLayout: UAV storage images, storage buffers,
        uniform buffers, samplers
      - [ ] 2.2 VkPipelineLayout for compute (vk_draw or vk_compute sibling)
      - [ ] 2.3 Map GCN resource tables to Vulkan descriptor bindings

      ### Phase 3 - compute pipeline + dispatch
      - [ ] 3.1 VkComputePipelineCreateInfo with compute stage
      - [ ] 3.2 Shader-cache-backed VkCreateComputePipelines
      - [ ] 3.3 vkCmdDispatch: bind pipeline + descriptors, push constants,
        dispatch
      - [ ] 3.4 IT_DISPATCH_INDIRECT via VkDispatchIndirectCommand

      ### Phase 4 - integration
      - [ ] 4.1 WalkCommandBuffer: replace WARN drop with dispatch exec
      - [ ] 4.2 Pipeline barrier for compute-to-graphics transitions
      - [ ] 4.3 Test with compute-using title or synthetic test

      ### Phase 5 - tests
      - [ ] 5.1 Compute shader translation round-trip tests
      - [ ] 5.2 Integration: dispatch to buffer readback to CPU verify
      - [ ] 5.3 PM4 golden test with compute dispatch


- [ ] H8. Storage images, mipmapped samplers, window/generic/vport
      scissor intersection in the draw executor.

      ## H8 implementation plan

      ### H8.1 - Storage images (shader read/write without samplers)
      - [ ] 1.1 Add VK_DESCRIPTOR_TYPE_STORAGE_IMAGE binding in draw executor
      - [ ] 1.2 Extend TextureEntry with storage flag: Storage usage in
        VkImageView, VK_IMAGE_LAYOUT_GENERAL
      - [ ] 1.3 In gcn_translate.cpp: image_load -> OpImageRead, support
        StorageImageReadWithoutFormat / WriteWithoutFormat capabilities
      - [ ] 1.4 Separate storage image bindings from sampled bindings
      - [ ] 1.5 Image layout transitions for storage images via barriers

      ### H8.2 - Mipmapped samplers
      - [ ] 2.1 Audit EnsureSampler: decode minLod, maxLod, lodBias,
        anisotropyEnable from GCN sampler word
      - [ ] 2.2 Decode full SamplerState fields in gfx10_state.cpp
      - [ ] 2.3 Texture upload: decode full mip chain, create VkImage with
        mipLevels > 1, upload each level
      - [ ] 2.4 VkImageView: set subresourceRange.levelCount from descriptor
      - [ ] 2.5 Verify with a 3D title using mipmapped textures

      ### H8.3 - Window / generic / viewport scissor intersection
      - [ ] 3.1 Identify three scissor sources in Uc shadow:
        screen_scissor_tl/br, PA_SC_GENERIC_SCISSOR,
        PA_SC_VPORT_SCISSOR(n)
      - [ ] 3.2 Compute intersection: max of mins, min of maxs across all
        three scissors; clamp zero-size
      - [ ] 3.3 Skip generic scissor when disabled (PA_SU_SC_MODE_CNTL bit)
      - [ ] 3.4 Apply final intersected scissor via vkCmdSetScissor
      - [ ] 3.5 Test pixel-perfect clipping vs SharpEmu reference



## From KytyPS5 & SharpEmu commit analysis (2026-07-22)

Reviewed 50+ KytyPS5 commits and 414 SharpEmu commits for patterns/features
not yet ported.  Sorted by estimated impact on game booting.

### HIGH — missing kernel primitives (may block game boot)

- [ ] P1. **Implement sceKernelSyncOnAddressWait/Wake** (SharpEmu #422).
      libKernel's address-wait primitives are unimplemented — every wait
      returns immediately and guest runtimes that build spinlocks/queues on
      top busy-spin forever.  This is a leading candidate for the Construct
      runtime's 31-thread worker pool race in Dreaming Sarah (H4).
      Files: new `src/hle/libkernel_syncaddr.cpp` (or extend libkernel_sync).

- [ ] P2. **Full SysV variadic float ABI (XMM0-XMM7 capture)** (SharpEmu #59).
      Our import trampoline spills only XMM0 (now via dispatcher.asm XMM save
      area) and never captures XMM1-XMM7 for variadic float args.  HLE
      handlers for variadic functions (printf family) cannot access float args
      past the first, so %f/%e/%g reads garbage and desynchronizes the
      argument stream.  The Construct runtime may call guest_vsnprintf with
      float arguments during JSON number formatting.
      Files: `src/hle/dispatcher.asm`, `src/hle/guest_printf.cpp`.

- [ ] P3. **BMI1/BMI2/ABM instruction emulation** (SharpEmu #249).
      Our AMD compat layer (src/cpu/amd_compat) covers SSE4a EXTRQ/INSERTQ
      and MONITORX/MWAITX but NOT BMI/BMI2/ABM (ANDN, BLSI, BLSMSK, BLSR,
      BEXTR, BZHI, TZCNT, LZCNT, RORX, SARX, SHLX, SHRX, PDEP, PEXT).
      Guest code using these crashes with STATUS_ILLEGAL_INSTRUCTION on CPUs
      that lack them.  Port from SharpEmu's BmiInstructionEmulator.

- [ ] P4. **TLS reservation audit: StartupStaticTlsReservation** (SharpEmu #454).
      Doubled StartupStaticTlsReservation and fixed NID BHouLQzh0X0 in
      SharpEmu to fix GTA V loading.  Our TLS block (headroom=0x10000,
      total=128KB) may be too small for some titles.  Check if the main
      thread's TLS allocation matches the doubled size.

### MEDIUM — system completeness

- [ ] P5. **Implement pthread semaphore exports** (SharpEmu #424).
      Our libkernel has no sem_init/sem_wait/sem_post/sem_destroy exports.
      Some games use semaphores for synchronization.

- [ ] P6. **Add RandomExports HLE** (SharpEmu #413).
      Linear-congruential / hardware RNG exports for games that seed RNG
      from the system entropy source.

- [ ] P7. **Add missing libc string exports** (SharpEmu #132).
      strchr, strrchr, memchr, strcat, strncat, strstr are referenced by
      some titles but not registered in our HLE.

- [ ] P8. **Implement pthread_yield NID** (SharpEmu #426).
      NID B5GmVDKwpn0 / pthread_yield is not registered.  Currently
      auto-stubbed (returns 0 / no-op), which is functionally correct but
      logs a warning.

- [ ] P9. **Fix lazy pthread object initialization** (Kyty 42d42e3).
      Kyty fixed a bug where pthread objects were lazily initialized
      incorrectly.  Review our pthread init sequence.

### LOW — features and compatibility

- [ ] P10. **Add ASTRO BOT compatibility stubs** (SharpEmu #481).
      Stubs for ContentExport, Font, and Pad calls Astro Bot needs.
      Not relevant to current titles but nice for future-proofing.

- [ ] P11. **Implement sceFontGetVerticalLayout** (SharpEmu #492).
      Font metrics HLE stub for games that query vertical font layout.

- [ ] P12. **Memory: back free pages of partially-overlapping fixed mapping**
      (SharpEmu #458).  Handles edge case in fixed-address memory
      allocation where a new reservation partially overlaps an old one.

- [ ] P13. **CPU: preserve guest return value across TLS lookup** (SharpEmu #104).
      Rare edge case where a guest TLS access inside an HLE handler could
      clobber the handler's return value before it reaches the caller.

## Done (2026-07-19/20)

M3.2b/c/d (runtime SGPR model, draw executor, present from GPU image);
splash pixels on screen; libSceAudioOut + waveOut; pad core + rumble/
touchpad/motion-neutral; AV hygiene (memcpy probes, VEH hardening,
sceSysmoduleIsLoaded); window responsiveness (guest worker thread);
UI flood-proof console; real boot-progress screen; no-arg UI launch;
BUILDING.md test list. See PROGRESS.md for details.
