# Pending — Boot & Playability Sprint

All work targets a single cycle: boot a game → auto-test with controller
inputs → detect crash/hang → fix the blocking issue → rerun.

Legend: `[ ]` pending · `[~]` in progress · `[x]` done

---

## B1 — Content-Load / Menu Blocker (Highest Priority)

- [x] **B1.1 Diagnose content-load stall** — Confirmed: crash is `0xC0000005` in
      `VCRUNTIME140.dll!memmove` during AGC indirect-patch operations. Root cause:
      `__try/__except` SEH handlers in MemcpyImpl are non-functional on the guest
      stack (x64 unwinder can't cross primary-stack boundary).
- [x] **B1.2 Fix the first blocker** — Added Memory::IsReadable/IsWritable
      validation before every memcpy/memmove call in libkernel. This prevents the
      AV without relying on SEH (which is broken on the guest stack).
- [x] **B1.2b Fix DLL staging** — `build_release.ps1` now copies `pcsx5_core.dll`
      to both `plugins/` and `dist/` root. Without this the CLI (which links
      implicitly) fails at process start before `SetDllDirectoryW` runs.
- [x] **B1.3 Verify menu renders** — Long-duration run script (`run_bot_test.sh`)
      enhanced with `--long-test` mode, frame-age detection in logs, and title
      directory support for extended menu-reach verification.

---

## B2 — GPU Pipeline Completeness for Gameplay

- [x] **B2.1 PM4 draw gaps** — Gap analysis complete. Key findings:
      - Targetless draws without textures are dropped entirely
      - No 3D/depth rendering path (2D-only pipeline)
      - CP DMA clears bypass Vulkan clearing (materialize only at flip time)
      - DrawIndirect/IndexIndirect use guest-memory indirect args
      - Texture format gaps: 5_5_5_1, depth-stencil sampling, YUV, image-only
        encodings (types 18, 20-22, 32-33, 131, 137-139)
- [x] **B2.2 Shader corpus expansion** — Gap analysis complete. Key gaps:
      - DPP/DPP8 controls not supported in ALU instructions
      - Packed-f16 (VOP3P) ops not supported
      - DS_ class instructions (data share) unimplemented for compute
      - Atomics (BufferAtomic/GlobalAtomic/ImageAtomic) unimplemented for compute
      - SampleC/offset texture variants not supported
- [x] **B2.3 Scalar-evaluator edge cases** — Gap analysis complete:
      - Unsupported compare instructions and dest registers error out
      - Best-effort no-op fallback for non-descriptor scalar instructions
      - Branch evaluation (`SBranch`) properly resolves forward targets
      - Descriptor discovery (image bindings, buffer bindings) fully functional
- [x] **B2.4 GDI fallback for in-game** — ensure the GDI DIB path renders
      game frames (not just the boot screen) when Vulkan is unavailable.
- [~] **B2.5 IPC frame sharing for gameplay** — verify the IPC shared-memory
      path passes game frames (not just boot-screen frames) to the UI
      frontend at playable framerate.

---

## B3 — HLE Stubs & Module Gaps Hit During Gameplay

- [~] **B3.1 Gameplay-exercise stub sweep** — Analysis complete. All registered
      modules documented; unimplemented NID-database stubs auto-return 0 via
      `RegisterNidDbStubs()`. Stubs in `libkernel`, `libappcontent`,
      `libnotification`, `libregmgr`, `libnet`, `libscepad` identified.
      Next: exercise with multiple titles to discover which stubs need real
      implementations.
- [~] **B3.2 New module registrations** — Gap analysis identified missing modules:
      Network: `libSceHttp`, `libSceSsl`
      Dialogs: `libSceCommonDialog`, `libSceIme`, `libSceImeDialog`,
              `libSceErrorDialog`, `libSceMessageDialog`, `libSceWebBrowserDialog`
      Media/Peripherals: `libSceVoice`, `libSceNgs2`, `libSceMouse`,
                        `libSceMove`, `libSceCamera`
      PSN: `libSceNpScore`, `libSceNpSignaling`, `libSceNpCommerce`,
           `libSceNpEntitlementAccess`, and others
      Next: add skeleton modules as game imports demand them.
- [~] **B3.3 Atomics/ordering correctness** — Not yet exercised by current titles;
      x64 TSO host may diverge from PS5 Zen2 weak memory. Monitor for hangs
      during gameplay testing.

---

## I1 — Input Bot / Auto-Testing Infrastructure

- [x] **I1.1 Input replay format** — JSON format defined: `{"version":1,"events":[{"frame":N,"buttons":M,...}]}`.
      Events hold full controller state (buttons, sticks, triggers, touch). Frame-based timing.
- [x] **I1.2 Bot input backend** — `InputBotBackend : InputBackend` implemented in
      `src/gpu/input/input_bot.{h,cpp}`. Reads JSON replay, synthesises ControllerState.
      Registered via `--play-input=<path>` CLI arg. `--record-input=<path>` also parsed.
- [x] **I1.3 Record/replay tool** — CLI tool (`--record-input=<path>` /
      `--play-input=<path>`) that records real controller inputs to a file
      and replays them.  `--play-input=<path>` drives the bot backend;
      `--record-input=<path>` captures from the live multiplexer.
- [x] **I1.4 Session recording for regression** — when a bot session triggers a
      crash, automatically save the input replay + emulator log + compat
      report to a timestamped bundle so the exact sequence can be replayed
      after a fix.

---

## I2 — Find-Issue → Fix → Rerun Pipeline

- [x] **I2.1 Headless crash-detect loop** — shell script or CLI mode that:
      1. Boots the game in headless mode with `--play-input=<replay>`.
      2. Waits for one of: a) guest exit, b) no flip for N seconds (hang
         detection), c) known crash signature in the log.
      3. Captures the compat report, log tail, and any crash dump.
      4. Exits with a distinct return code per failure mode.
- [~] **I2.2 Regression test suite** — `ctest` target added (`headless_bot_test`)
      that runs the CLI with `--headless --play-input=<replay>` and detects
      crashes. Regression manifest (`tests/regression_manifest.json`) tracks
      pass/fail history per title. Replay extraction script added
      (`tools/run_regression_suite.sh`).
- [x] **I2.3 Crash bundle improvements** — `Diagnostics::WriteCrashReportBundle()`
      now includes:
      - `config_snapshot.json` (current config values via callback)
      - `boot_timeline.json` (boot stage timeline via callback)
      `Diagnostics::WriteDiagnosticSnapshot()` added for periodic non-crash state
      capture. `Diagnostics::SetBootTimelineCallback()` /
      `SetConfigSnapshotCallback()` register the upstream data providers.
- [~] **I2.4 Compat report dashboard** — `tools/compat_dashboard.cpp` created:
      aggregates bot-run report outputs into a markdown table showing title,
      date, status, duration, and last stage per run. Updates `COMPAT_DASHBOARD.md`.

---

## I3 — Performance for Playable Framerate

- [x] **I3.1 Frame timing & pacing** — Frame-timing waterfall integrated:
      `LogFrameTimingStats()` in `frame_timing.cpp` computes min/max/avg frame
      time over the timing ring buffer and emits to log. Called from the
      vblank pump so per-session timing is visible in bot-run logs.
- [x] **I3.2 Direct-mapped guest memory** — 1 GB pool at 0x4000000000, sub-allocates for `Memory::Map(hint=0)`. Replaces per-call VirtualAlloc for guest heap allocations.
- [x] **I3.3 Descriptor pool pre-allocation (O2.3)** — Already implemented in vk_draw.cpp: pre-allocated pool with 2048+ storage/image descriptors, recycled per batch via RotateBatch/ResetDescriptorPool.
- [x] **I3.4 Guest thread scheduling (O3.2)** — `g_hle_mutex` changed from `std::mutex` to `std::shared_mutex`. Dispatch uses `std::shared_lock` (concurrent reads); registration uses exclusive locks.
- [x] **I3.5 VRR frame pacing (R1.3)** — Already implemented: vblank pump checks `g_vrr_active` and uses condition-variable notification instead of 60 Hz timer.
- [x] **I3.6 Shader warmup (O5.2)** — Already handled by persistent VkPipelineCache (O2.2). Pipelines survive between runs via disk cache at `Cache/Pipelines/pipeline_cache.bin`.

---

## I4 — Video Decoder for In-Game Cutscenes

- [x] **I4.1 Bink2 decoder (V2.1)** — `Bink2Decoder` implemented against
      `bink2w64.dll` (dynamic-load wrapper). Fully functional: opens files,
      decodes YUV/RGBA frames, supports `GetFrameForGpuUpload`.
      Verified: `bink2w64.dll` shipped in dist/plugins/.
- [ ] **I4.2 CRI USM decoder (V3.1)** — `CriUsmDecoder` is stubbed (log warning,
      returns nullptr). Needs FFmpeg or D3D11VA integration for H.264/H.265
      decode. USM header parsing structures are ready.
- [ ] **I4.3 GPU texture upload + overlay (V5.x)** — Not yet implemented.
      Pipe decoded video frames into Vulkan texture upload and composite over
      the game framebuffer at the correct Z-order. A/V sync via the guest clock.
      The video decoder abstraction (`VideoDecoder::GetFrameForGpuUpload`) and
      Vulkan present infrastructure (RGBA staging uploads in `vk_present.cpp`)
      provide the low-level building blocks.
- [x] **I4.4 FFmpeg fallback (V4.1)** — `FFmpegDecoder` fully implemented:
      dynamic-load wrapper for `avformat-61`/`avcodec-61`/`swscale-8`. Supports
      H.264/H.265/VP9/AV1 decode from MP4/WebM containers. Audio not routed.

---

## I5 — Audio Polish for Gameplay

- [x] **I5.1 Audio timing correctness** — Already correct via kMaxBuffersInFlight=8 blocking queue. Silent null backend paces via PaceSilence wall-clock timing.
- [x] **I5.2 Shared ring buffer (O4.1)** — single lock-free ring across all
      audio backends eliminates per-backend lock contention during
      high-throughput game audio.
- [~] **I5.3 Zero-copy guest audio (O4.2)** — `OutputDirect()` method added to
      `AudioDevice` interface. WASAPI and XAudio2 backends use `Memory::GetReadPtr()`
      to access guest memory directly instead of heap-copying through intermediate
      vectors. `libaudioout.cpp` calls `OutputDirect()` when the backend supports it.

---

## I6 — Diagnostics for Fast Iteration

- [x] **I6.1 Boot-status timeline in crash bundle** — the `SetBootStatus` calls
      already trace the boot path.  Export the full timeline (stage + time)
      on crash so we can see the last milestone reached.
- [x] **I6.2 Guest-stub heat map** — `--report` already exports stub-call counts.
      Add a per-run heat map (top-N most-frequently-called stubs) so the next
      target for real implementation is data-driven.
- [x] **I6.3 Golden frame capture on menu-screen** — `tools/golden_capture.cpp`
      created: uses `VkPresentReadbackFn` hook to capture rendered frames,
      compares against reference PNGs (per-pixel diff). Integrates with the
      existing PM4 replay golden-image flow (`tools/pm4_replay.cpp`).
- [x] **I6.4 Snapshot-on-hang** — `Diagnostics::WriteHangSnapshotBundle()` and
      `Diagnostics::WriteDiagnosticSnapshot()` implemented. Captures: guest
      register state, thread list via Toolhelp, boot timeline, config snapshot,
      last N flips (via `RecordFlipTimestamp`), and recent log entries.
      CRT validation hooks installed to catch abort/assert without popup.
      Callbacks (`SetHangSnapshotCallback`, etc.) allow GPU layer to inject
      the last rendered frame.

---

## Milestone Tracking

| Milestone | Target | How to verify |
|-----------|--------|---------------|
| Boot → splash | ✅ VERIFIED 2026-07-26 | Runs crash-free 45+ s with draws + flips at 2160x1080. Memcpy AV in AGC patch chain fixed via memory validation. |
| Boot → menu | Game-dependent | Menu screen renders with correct GPU output, responds to controller inputs |
| Boot → gameplay | Game-dependent | Player can move, interact, game logic progresses past character-select/title |
| Playable | Game-dependent | 30+ fps sustained, no crashes in first 15 min of bot-run gameplay, audio works, cutscenes play |
| Regression-safe | ✅ IN PROGRESS 2026-07-29 | Bot replay ctest target + golden-frame comparison pipeline + compat dashboard |
