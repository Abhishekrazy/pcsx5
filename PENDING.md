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
- [x] M2. WASAPI backend for libSceAudioOut (config backend=1): shared-mode,
      event-driven IAudioClient render in src/hle/libaudioout.cpp, with
      fallback to waveOut (+ WARN) when init/mix format is unsuitable;
      backend=2 (XAudio2) still TODO — currently maps to waveOut.
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
      fill the ScePadData touch block (@0x20) and accel (@0x0C). Haptics /
      adaptive triggers (output reports) NOT done. UNTESTED ON HARDWARE —
      no DualSense attached during development; verified by compile +
      no-device fallback only, runtime verification needs a real controller.
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
- [ ] H4. Verify menus render after H1-H3; compare against SharpEmu
      golden dumps; fix remaining format/tiling/blend mismatches.

## High priority — later GPU work (after menus)

- [ ] H5. Detiling + BC texture formats (needed for 3D titles).
- [ ] H6. Compute queue support.
- [ ] H7. Shader cache (disk) + async pipeline compilation.
- [ ] H8. Storage images, mipmapped samplers, window/generic/vport
      scissor intersection in the draw executor.

## Done (2026-07-19/20)

M3.2b/c/d (runtime SGPR model, draw executor, present from GPU image);
splash pixels on screen; libSceAudioOut + waveOut; pad core + rumble/
touchpad/motion-neutral; AV hygiene (memcpy probes, VEH hardening,
sceSysmoduleIsLoaded); window responsiveness (guest worker thread);
UI flood-proof console; real boot-progress screen; no-arg UI launch;
BUILDING.md test list. See PROGRESS.md for details.
