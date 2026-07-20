# Pending — prioritized work queue

Low/medium priority first; high-priority items broken into small,
independently verifiable pieces. Completed Phase 5 M3 details moved to
PROGRESS.md / ROADMAP.md.

## Low priority (quick wins, do first)

- [x] L1. Audio/pad unit tests: audio ring/port-state tests for
      libaudioout (open/output/volume/close state machine, paced-silence
      timing) and pad state machine tests (handle validation, error codes,
      scePadRead ring drain). Pattern: tests/hle_phase3_tests.cpp.
- [x] L2. Trophy stubs -> basic unlock-event logging
      (ROADMAP Phase 7; log-only, feeds compat reports).
- [x] L3. Keystone parsing/validation stubs (ROADMAP Phase 7).
- [x] L4. Fullscreen toggle (F11) + aspect-correct scaling for the
      emulator window (ROADMAP Phase 6b).
- [x] L5. Multi-user profile model in config (ROADMAP Phase 7).
      Done: UsersConfig section (profiles + active_user) in
      src/config/config.h/.cpp, schema v3, JSON persistence, accessors
      ActiveUserProfile/FindUserProfile/ActiveUserId. Deferred: HLE wiring —
      libpad.cpp and libuser_service.cpp still hardcode their user ids (read
      only per task rules; they should consume the new ConfigService
      accessors later). Config tests not extended: tests live in
      tests/config_tests.cpp which is owned by another agent.

## Medium priority

- [x] M1. Memory: reserve large direct-memory region up front and
      sub-allocate, instead of one VirtualAlloc per guest request —
      the content-load phase makes ~450+ 64KB allocations and takes 8+
      minutes; batching should cut that drastically. (This is also the
      "64KB allocation" the user asked about.)
      Done: measurement first (.work/m1_bench.cpp) showed the syscall path
      was never the bottleneck — 450x64KB VirtualAlloc = 0.58 ms total,
      sub-alloc from one reservation = 0.50 ms; the 8+ minutes is guest-side
      pacing, not allocation cost. The phys pool already reserved 2 GB once;
      the remaining hot-path cost was per-64KiB demand-commit VEH faults
      (one exception + one Info log line per block). Fix in
      src/hle/libkernel.cpp: commit the phys pool in 16 MiB chunks ahead of
      the bump pointer (EnsurePhysCommitted, called from both
      AllocateDirectMemory handlers); per-fault logs demoted Info->Debug in
      libkernel.cpp CommitPhysPool and src/memory/memory.cpp CommitOnFault.
      Memory:: public API unchanged. Synthetic 450-block burst, fully
      touched: 4.65 ms before (fault path + logging) -> 2.83 ms after
      (.work/m1_bench2.cpp), and zero exceptions/log lines per block now.
      Tests: memory suite lives in tests/ (owned by other agents) — skipped
      per task rule.
- [x] M2. WASAPI + XAudio2 backends for libSceAudioOut (config backend=1/2):
      shared-mode, event-driven IAudioClient render and an XAudio2 2.9
      source-voice pool, both in src/hle/libaudioout.cpp; each falls back
      (WASAPI -> waveOut / XAudio2 -> WASAPI -> waveOut) with WARNs.
- [x] M3. ATRAC9 HLE codec path: route game ATRAC9 decode calls through
      vendored LibAtrac9 (UI preview player already proves decode).
      Done: new src/hle/libatrac9.cpp registers libSceAtrac9 handlers
      (sceAtrac9InitHandle with RIFF/AT9 header parsing + raw 4-byte config,
      sceAtrac9ReleaseHandle, sceAtrac9Decode -> PCM16 one frame per call,
      sceAtrac9GetInfoType codec/bitrate/superframe/frame-samples queries,
      sceAtrac9GetInternalErrorInfo) backed by a mutex-guarded handle
      registry over LibAtrac9. Registered via RegisterLibAtrac9() in
      hle.cpp; libatrac9.cpp added to all 10 HLE source lists in
      CMakeLists.txt and the atrac9 static lib moved to top level and
      linked into pcsx5 + the 9 HLE test targets. Unknown sceAtrac9*
      exports keep auto-stub behaviour. Build /W4 /WX clean, ctest 32/32.
- [x] M4. DualSense HID input path: real touch finger positions, motion
      sensors, haptics/adaptive triggers (reference:
      src/ui_csharp/WindowsDualSenseReader.cs; keep XInput fallback).
      Native HidD/SetupAPI reader in src/gpu/dualsense_hid.h (background
      thread, buttons/sticks/triggers/touch fingers/gyro+accel), merged into
      GPU::PollEvents pad state — DualSense supersedes XInput when attached;
      keyboard + XInput fallback unchanged. Touch fingers + acceleration now
      fill the ScePadData touch block (@0x20) and accel (@0x0C). UNTESTED ON
      HARDWARE — no DualSense attached during development; verified by
      compile + no-device fallback only, runtime verification needs a real
      controller.
- [x] M4b. DualSense haptics + adaptive triggers via HID output reports
      (Phase 6): USB report 0x02 / BT report 0x31 (seq tag + CRC32 seeded
      0xA2) built in src/gpu/dualsense_hid.h — rumble motors + trigger
      effect blocks (off/feedback/weapon/vibration, clamped params), sent
      via WriteFile on the reader's handle (read-only fallback handled
      gracefully). GPU::SetPadVibration drives DualSense motors when
      connected (XInput otherwise); new GPU::SetPadAdaptiveTrigger.
      scePadSetTriggerEffect parses the retail ScePadTriggerEffectParam
      (triggerMask + per-side mode/paramData[10]) and routes through it.
      UNTESTED ON HARDWARE — compile + no-device fallback verified only.
- [x] M5. Embed game window inside the library window (SharpEmu-style):
      emulator prints its HWND, UI reparents it via SetParent/HwndHost,
      strips decorations, keeps Stop + Console buttons visible while
      running. Design agreed; focus/resize are the known gotchas.
      DONE: emulator prints PCSX5_WINDOW_HANDLE=<decimal HWND> after window
      creation and accepts --embed (window starts hidden; CLI default
      unchanged). WPF parses the line from stdout, reparents the HWND into
      an HwndHost (custom black-background class, WS_CHILD, caption/borders
      stripped), letterboxes to the game's aspect on layout change, and
      shows a game view with bottom bar (title + Console toggle + Stop).
      Console panel reuses the existing ConsoleOutputTextBox as a side
      panel (HwndHost airspace makes overlays impossible). Boot screen is
      repainted every PumpWindowEvents while active so it survives the
      reparent. Verified end-to-end with Dreaming Sarah (PPSA02929):
      boot screen renders inside the UI, Stop kills cleanly, library
      returns, crash-mid-embed keeps the UI alive. Known limits: Vulkan
      swapchain is created at 1280x720 and is NOT recreated on embed
      resize (GDI path stretches fine); F11 fullscreen is a no-op when
      embedded (window is a child); keyboard focus relies on click-to-
      focus of the reparented child (SetFocus is issued at embed time).
- [x] M6. Command batching in vk_draw executor (one submit+fence per
      draw today — batch per command buffer, sync at flip).

## High priority — upstream SharpEmu ports (sync 0f224ec -> ac883e4, 27 commits)

Port these from sharpemu_clone (now fetched; diff each commit and
transliterate to our C++ model). Ordered by value to the menu push.

- [x] U1. sRGB encode of linear-float flips at present (327018e) — wrong
      gamma/washed-out menus without it. vk_present.cpp.
      DONE (2026-07-20): linear-float sources (R16G16B16A16/R32G32B32A32
      SFLOAT RTs) blit through a cached swapchain-sized sRGB intermediate
      (sRGB store encodes) then same-class vkCmdCopyImage into the UNORM
      swapchain. VkPresentFromImage gained a src_format param (plumbed
      from VkDrawLookupRenderTarget via vulkan_backend.cpp flip path).
      Tests: new tests/vk_present_format_tests.cpp (ctest
      vk_present_format), mirroring upstream's encode-format tests.
- [x] U2. Refresh CPU-rewritten guest textures by write generation
      (04557fd) — stale textures otherwise. vk_draw texture upload +
      memory write tracking.
      DONE (2026-07-20): Memory::TrackGuestWrites/UntrackGuestWrites/
      TryGetGuestWriteGeneration/RearmGuestWrites (src/memory/memory.*) —
      ranges armed PAGE_READONLY, first CPU write faults through the
      guest-fault VEH (disarm, generation++, resume) ahead of
      demand-commit. vk_draw TextureEntry records the generation at
      upload; VkDrawExecute skips re-upload while it matches. Tests:
      tests/memory_write_tracker_tests.cpp.
- [x] U3. Read mip 0 from its GFX10 mip-chain offset (6ee445f) — texture
      upload correctness. gfx10_state DecodeImageDescriptor.
      DONE (2026-07-20): DecodeImageDescriptor decodes MAX_MIP into
      ImageDesc.mip_levels; GetBaseMipPlacement (AddrLib chain-offset
      math, incl. tail-resident mip 0 with bit-deinterleave) +
      DetileTailMip0 in gfx10_state; vk_draw UploadTexture reads mip 0
      at guest_addr + byte_offset; TextureIdentity hashes mip_levels.
      Tests: TestImageDescriptorMipLevels / TestBaseMipPlacement /
      TestDetileTailMip0 in gfx10_state_tests.
- [x] U4. VideoPresenter logical width/height fix (ac883e4) — present
      sizing. vk_present/vk_draw RT model.
      NO-OP BY DESIGN: upstream fix addresses its physical-vs-logical RT
      size split (resolution scaling); our RenderTargetEntry has a
      single always-populated width/height — the bug class does not
      exist in our architecture. Verified against the upstream diff.
- [x] U5. Mutex ownership handoff to head waiter (73e8821) + UE-title
      boot fix (90c72eb) — libkernel_sync.cpp.
      DONE (2026-07-20): GuestMutex rebuilt (owner/recursion/type/FIFO
      waiter deque + condvar); unlock hands ownership directly to the
      head waiter (MutexHandoffLocked). Mutex types real:
      scePthreadMutexattrInit/Settype/Destroy store normalized type
      (0/1 ErrorCheck, 2 Recursive, 3 Normal, 4 AdaptiveNp); AdaptiveNp
      self-relock is idempotent (the UE boot fix); static initializer
      word==1 promotes to adaptive. CondWaitImpl adopts/releases/
      re-acquires per upstream. NOT ported: PhysicalVirtualMemory /
      TrackedCpuMemory / memcpy fast path (host perf work, no bearing on
      mutex semantics). Tests: kernel_sync_tests (+mutex types, handoff,
      stress).
- [x] U6. Emulate AMD-only Zen 2 instructions in software (8ef5a54) —
      guest crash on Intel hosts without it. cpu/VEH illegal-instruction
      path.
      DONE (2026-07-20): new src/cpu/amd_compat.{h,cpp} (hand-rolled
      decoder — upstream used Iced — + Extract/InsertBitField semantics);
      kernel.cpp VEH claims STATUS_ILLEGAL_INSTRUCTION after the INT3
      block: MONITORX no-op, MWAITX yield, EXTRQ/INSERTQ immediate forms
      emulated via CONTEXT XMM slots, RIP advanced; invalid encodings
      fall through to the crash path. Tests: sse4a_bitfield_tests
      (42 checks).
- [x] U7. POSIX file syscalls return -1/errno on failure (bb3318a) —
      libkernel file I/O error convention.
      DONE (2026-07-20): sce file handlers factored into cores returning
      raw Orbis errors; POSIX exports open/close/read/write/stat/fstat
      wrap them via PosixFailure (-1 + guest errno through the shared
      __error() cell, exposed as HLE::GuestErrnoPtr/SetGuestErrno in
      liblibc.cpp). Real bug found: UCRT terminates on _close of a dead
      fd, so a tracked-fd set rejects bad/closed fds with EBADF before
      the CRT. Tests: tests/libkernel_file_tests.cpp.
- [x] U8. Missing NIDs (0c467e8) — merge into assets/nid_db.txt
      (validate each with the name->NID hash).
      DONE (2026-07-20): 29/29 NIDs validated against tools/nid_scan.cpp
      NameToNid (salted PS5 form) and merged (745->774 lines; libKernel
      25, Agc 1, NpManager 1, NpTrophy2 1, incl. sceKernelMapDirectMemory2
      for U9). nid + nid_db tests pass.
- [x] U9. sceKernelMapDirectMemory2 (d7f6e3f).
      DONE (2026-07-20): MapDirectMemoryCore extracted from v1 impl; v2
      takes memoryType@rdx (accepted, unused), shifted prot/flags/
      physStart, alignment from 7th stack arg (GuestArgs::stack_args).
      NID BQQniolj9tQ registered. Covered in libkernel_file_tests.
- [x] U10. Sample 2D array textures with real layers (25d741b) — needed
      for 3D titles; pair with H5.
      DONE (2026-07-20): ImageDesc.depth (word4[12:0]+1) +
      BaseMipPlacement.chain_slice_bytes in gfx10_state;
      GcnSpirvImageBinding.is_arrayed + GcnIsArrayedImageBinding rule in
      gcn_translate (arrayed OpTypeImage, vec3 (u,v,slice) coords,
      explicit LOD at +3, resinfo ivec3->ivec2); libagc sets the flag per
      stage and fills arrayed_view/depth; vk_draw uploads layered sources
      as one 2D-array image (per-slice detile at layer*chain_slice_bytes
      + mip0 offset) with 2D_ARRAY views; shader-cache key includes the
      flag (cache version 1->2). ImageGather4 path noted for when the
      translator supports gather. Tests: TestImageDescriptorDepth,
      chain_slice_bytes cases, TestTranslateArrayedImageSample.

## High priority — M3.3 menus (broken into pieces)

Context: splash renders correctly; after ~180 frames the guest enters a
content-load phase (no draws), then the run dies silently ~8-10 min in.

- [~] H1. Fix the silent death: reproduce with full logging, catch where
      the process dies (suspected host stack exhaustion in recursive
      VEH/TLS handling, or external kill). Add a heartbeat/watchdog log
      so the death point is visible. Small: instrument first, fix second.
      DIAGNOSED (2026-07-20): the silent death is a process abort() from
      the guest C++ throw/terminate path at ~850 direct-memory
      allocations — the game's throw hits the HLE unwind path, no catch
      handler is found (H3 work-in-progress in src/hle/liblibc.cpp), and
      the process aborts.  Confirmed twice: patched TLS build aborts at
      852 allocs / 20 s, unpatched build (PCSX5_NO_TLS_PATCH=1) aborts
      identically at 850 allocs / 779 s — the old ~8-10 min death is the
      same event at trap-throttled speed.  abort() bypassed every
      installed handler, hence no log line and no crash bundle.
      Instrumentation landed in src/kernel/kernel.cpp: 30 s heartbeat
      thread, CRT death hooks (SIGABRT/invalid-parameter/purecall +
      _set_abort_behavior — caught the abort with a final log line),
      SetThreadStackGuarantee(64KB), STATUS_STACK_OVERFLOW log branch,
      and fixed HostUnhandledExceptionFilter chaining to the diagnostics
      crash-bundle writer (host crashes never wrote bundles before).
      Remaining: the FUNCTIONAL fix is the concurrent agent's H3 unwind
      work (the game expects its catch handler to run).  Side finding:
      a VS-18-preview CRT hardened-delete false positive fastfails
      process teardown on Win11 segment heaps (seen in
      syscall_validation and in pcsx5 error-exit paths, both with and
      without my changes — pre-existing, not app heap corruption).
- [x] H2. TLS-emulation storm performance: ~9,300 first-chance AVs per
      boot from fs-relative TLS emulation — each goes through the VEH.
      Cache resolved TLS patch sites (patch once, execute natively)
      instead of trapping every access.
      DONE (2026-07-20): new src/kernel/tls_patch.{h,cpp} — on first
      trap at a guest RIP the instruction is rewritten to `call stub`;
      the per-site stub loads the current thread's guest thread pointer
      from a host TLS slot via gs:[TEB TlsSlots] and performs the
      access natively (SharpEmu-style, per-thread correct, flags and
      registers preserved; rsp-relative forms excluded; torn-patch
      retry + stub-fault restore-and-emulate safety paths; sites must
      match Kernel::ResolveGuestThreadPointer semantics — binding the
      CpuCore tls_base directly corrupted worker threads).  Metrics
      (Dreaming Sarah boot): TLS traps 336,423/420 s -> 93 total
      (one per unique site), time to first flip 25.95 s -> 11.03 s
      (2.4x).  Caveat: the full implementation compiles only into
      pcsx5 (PCSX5_TLS_PATCH_FULL) because its inert presence in test
      binaries toggles a toolchain CRT fastfail at exit (see H1);
      PCSX5_NO_TLS_PATCH=1 env var disables patching at runtime.
- [x] H3. __cxa_allocate_exception / guest C++ exception support
      (hit at ~850 allocations in the earliest attempt): implement the
      libc++abi throw path enough for GameMaker titles.
      REAL (src/hle/liblibc.cpp): __cxa_allocate_exception/free_exception
      (libc++abi header layout), __cxa_throw/rethrow, __cxa_begin_catch/
      end_catch bookkeeping, __cxa_get_exception_ptr,
      __cxa_current_exception_type, __cxa_get_globals{,_fast},
      __cxa_guard_acquire/release/abort (thread-safe interlock), and a real
      two-phase Itanium unwinder over the guest's own .eh_frame
      (PT_GNU_EH_FRAME parsed by the loader into
      LoadedModule::eh_frame_hdr_addr, handed to HLE from main()). The
      unwinder interprets DWARF CFI, calls the guest's own personality
      routine via a new 6-arg InvokeGuestFunction6 (dispatcher.asm), and
      resumes at landing pads through an emitted register-restore
      trampoline written over the dispatcher's guest return slot.
      _Unwind_RaiseException/Resume + the context accessors (GetGR/SetGR/
      GetIP/SetIP/GetLSDA/GetRegionStart/GetCFA/GetTextRelBase) are real.
      SURVIVING (log, sane value, no silent wrong return): HLE
      __gxx_personality_v0 (only used if a guest imports it instead of
      linking libc++abi — returns CONTINUE_UNWIND; guests with their own
      personality, like Dreaming Sarah, are unaffected), _Unwind_Backtrace/
      ForcedUnwind/Find_FDE/FindEnclosingFunction/GetBSP (log+0),
      __cxa_pure_virtual/bad_cast/bad_typeid/call_unexpected family
      (log + guest abort, matching native semantics). Unhandled exception
      = std::terminate semantics (guest exit 134). Tests:
      tests/libcxxabi_tests.cpp (ctest `libcxxabi`). Full unwind path
      needs in-game verification (Dreaming Sarah content-load).
- [~] H4. Verify menus render after H1-H3; compare against SharpEmu
      golden dumps; fix remaining format/tiling/blend mismatches.
      IN PROGRESS (2026-07-20): run now reaches content load with the
      splash flipping continuously; several real blockers found and fixed
      (below).  MENUS NOT YET REACHED — current blocker is a deterministic
      game-data failure (see end of entry).
      Fixes landed:
      1. src/hle/liblibc.cpp + src/hle/hle.{h,cpp}: the eboot's CIE
         personality pointer is an imported __gxx_personality_v0 bound to
         an HLE thunk (Dreaming Sarah does NOT statically link it —
         H3's assumption was wrong).  The unwinder now detects thunk-page
         personalities (HLE::IsHleThunkAddress) and evaluates the gcc
         LSDA NATIVELY (EvalLsda/ReadTtypeEntry/ReadThrownTinfo: call-site
         table scan, action records, TType matching, catch(...) and
         pointer-equal type match) instead of calling the stub, which
         always returned CONTINUE_UNWIND -> terminate.  Two-phase search
         + cleanup landing works; FrameEval carries signal_frame.
      2. src/cpu/cpu.cpp: Kernel::RegisterThread was never called for
         guest-created threads, so ResolveGuestThreadPointer fell back to
         the shared main-thread TLS block for EVERY worker — all worker
         fs:[0] reads aliased the main thread pointer (per-thread runtime
         caches collided).  ThreadEntrypoint now registers the thread in
         the kernel map before TlsPatch::BindCurrentThread.
      3. src/hle/libkernel.cpp (scePthreadCreate): worker TLS block was
         0x4000 bytes with tp at block start; Orbis libc/CRT data lives
         at NEGATIVE offsets from tp (down to tp-0x1648), so the first
         negative-offset store faulted.  Now allocates 0x10000 headroom
         below tp, mirroring the main thread's TLS block.
      4. src/hle/libkernel.cpp (_Getptolower) + src/hle/liblibc.cpp
         (_Getpctype): both built their shared ctype tables lazily with
         an unsynchronized check — a concurrent caller could observe the
         table pointer/flag set before the fill completed and read a
         zeroed table.  Both now use std::call_once.
      Verified: throw site is uncaught per .eh_frame + full 32KB stack
      scan (no catching frame exists — H1's "game expects a catch" was
      wrong for this throw; the exception is a genuine fatal runtime
      error).  The game throws "type must be number, but is null" (its
      own message, recovered from the exception object) on a
      "BackgroundLoader P.Worker" thread while building a runtime sprite
      record from the parsed data.js (Construct-runtime game, not
      GameMaker) tree.  The failing element corresponds to a FRACTIONAL
      JSON value (0.5); the JSON tree node holds null there.  Patching
      data.js 0.5->1 moved the failure to the next fractional value;
      rounding ALL floats to ints delayed the abort from ~7-9s to ~23s
      (then a different null-deref crash, likely from mangled data).
      Failure timing varies (6.8s/19.5s/24s) with thread scheduling;
      8-core affinity avoids the throw but stalls before graphics init.
      CURRENT BLOCKER: the game's Construct runtime stores null for
      fractional numbers in its data.js tree (or a producer/consumer
      race in its 31-thread P.Worker job pool leaves tree nodes
      half-built).  Whether this is a JSON number-lexing failure
      (deterministic content bug) or a job-system race is unresolved;
      no HLE stub, ctype, or syscall mismatch remains implicated.
      Diagnostics left in src/hle/liblibc.cpp for the next session:
      throw-type/message dump, failing-variant tag dump, parser-frame
      and array-element dumps, .work/guest_text.bin guest-text snapshot
      (6MB, disassembled with python capstone).

## High priority — later GPU work (after menus)

- [x] H5. Detiling + BC texture formats (needed for 3D titles).
      (2026-07-20) gfx10_state: full (data_format, number_format) ->
      VkFormat table covering every pair the unified decoder can emit
      (unsupported pairs explicit), native BC1-BC7 VkFormats (169-182),
      DetileSurface port of SharpEmu GnmTiling.cs (verified modes 1/4/5/8/
      9/24/27: 256B_S, 4K_Z/S, 64K_Z/S, 64K_Z_X/R_X exact RB+ patterns),
      TiledSurfaceByteCount (whole-swizzle-block spans); vk_draw upload
      path detiles tiled surfaces and uploads BC at 4x4-block granularity
      (linear path unchanged). gfx10_state_tests 750 checks.
      Handoff (other agents' files): libagc.cpp must plumb
      ImageDesc.tile_mode into VkDrawTexture.tile_mode (field added,
      defaults 0 = linear); gcn_eval.cpp's unified table stops at 154 —
      the 169-182 BC passthrough entries from SharpEmu
      Gfx10UnifiedFormat.cs still need porting before BC textures reach
      the draw executor.
- [~] H6. Compute queue support.
      Outcome 2026-07-20 (wave 2): packet plumbing is complete —
      `sceAgcCbDispatch`/`sceAgcDcb/AcbDispatchIndirect` emit
      IT_DISPATCH_DIRECT/INDIRECT packets, the submit walker parses and
      counts them, and COMPUTE_PGM_LO/HI (kComputePgmLo/Hi) is tracked in
      the Sh shadow.  Log sweep of every capture in .work/*.log shows
      zero dispatches from the 2D titles, so the executor is DEFERRED:
      dispatch packets now log a clear WARN ("NOT IMPLEMENTED ... H6;
      dispatch dropped", includes group dims + CS address) instead of a
      silent INFO.  What's missing for the full executor: CS shader
      discovery/translation (GcnSpirvStage::Compute in gcn_translate),
      DS/global-memory/buffer-store atomic support in the translator
      (rejected by design today), compute VkPipeline + descriptor sets
      in vk_draw (or a vk_compute sibling), vkCmdDispatch.
- [x] H7. Shader cache (disk) + async pipeline compilation.
      Disk cache done 2026-07-20: `src/gpu/shader_cache.{h,cpp}` —
      128-bit content key over (GCN shader words + all
      output-affecting GcnTranslateOptions), atomic tmp+rename stores,
      magic/version/size-validated loads under `Cache/Shaders/`, plus
      `GcnTranslateWithCache` (gcn_translate.h) and an off-thread
      cache warm-up worker (`GcnShaderCacheWarmupAsync`).  Tests in
      tests/shader_tests.cpp (round-trip, key sensitivity, corruption,
      concurrency, hook, warm-up).
      Wired 2026-07-20 (wave 2): libagc's AgcBuildDrawProgram now
      translates via `GcnTranslateWithCache` against a shared
      `GcnShaderDiskCache{"Cache/Shaders"}` (unconditional; store
      failures non-fatal, corruption treated as a miss) — warm second
      boots skip retranslation.
      Remaining: async VkPipeline creation in vk_draw consuming the
      warmed disk cache.
- [ ] H8. Storage images, mipmapped samplers, window/generic/vport
      scissor intersection in the draw executor.

## Done (2026-07-19/20)

M3.2b/c/d (runtime SGPR model, draw executor, present from GPU image);
splash pixels on screen; libSceAudioOut + waveOut; pad core + rumble/
touchpad/motion-neutral; AV hygiene (memcpy probes, VEH hardening,
sceSysmoduleIsLoaded); window responsiveness (guest worker thread);
UI flood-proof console; real boot-progress screen; no-arg UI launch;
BUILDING.md test list. See PROGRESS.md for details.

## Done (2026-07-21)

- [x] V1. PM4 capture/replay + golden-image tests (ROADMAP Phase 5
      validation item). Capture: additive hook in libagc.cpp
      WalkCommandBuffer (env PCSX5_PM4_CAPTURE=<dir>, cached
      GetEnvironmentVariable lookup — one branch when unset) dumping
      submit_NNNNN.bin (raw PM4 dwords) + submit_NNNNN.json (queue,
      guest addr, dwords, cx/sh/uc shadow + index state). Replay:
      tools/pm4_replay.cpp links the same HLE+GPU objects as
      hle_agc_tests, maps recorded guest memory at its captured
      addresses, submits via the registered sceAgcDriverSubmitDcb
      symbol, opens a real videoout port, patches RFlip handles, and
      presents on a hidden window (GPU::SetEmbeddedMode). Readback:
      additive vk_present hook (VkPresentSetReadbackHook) copies the
      presented source image (guest FB texture / render target) to a
      host buffer and delivers unscaled BGRA8 per flip. Golden test:
      `pm4_replay record-synth` builds a self-contained 3-frame
      synthetic capture (CP DMA fill + DMA copy pattern band + RFlip
      per frame, 320x180 BGRA) under tests/golden/pm4_synth with
      golden PNGs (2.5 KB each); ctest `pm4_golden` replays and
      compares (tolerance 8/255/channel, <=0.5% pixels — DMA frames
      compare exact). GPU-less hosts exit 0 with PM4_GOLDEN_SKIP
      unless PCSX5_GOLDEN_REQUIRED=1. Verified locally: 3/3 golden
      MATCH on the NVIDIA ICD, full ctest green except the concurrent
      agent's in-flight gfx10_state detile work. Game captures
      (stream + shadow only, no memory manifest) are analysis-only;
      replayable captures come from record-synth.
