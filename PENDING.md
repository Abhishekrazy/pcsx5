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
- [x] **B1.3 Verify boot-to-splash** — After B1.2, Dreaming Sarah runs crash-free for
      45+ seconds with consistent draws+flips at 2160x1080. Next: long-duration
      run (`>5 min`) to confirm it reaches the menu. Also verify with other titles
      (LOST EPIC, Jusant).

---

## B2 — GPU Pipeline Completeness for Gameplay

- [ ] **B2.1 PM4 draw gaps** — as the game progresses past the splash, new PM4
      command sequences (different Cx/Sh/Uc register combinations, blended
      draws, depth-only passes, unbounded color targets) will be encountered.
      Add handlers for each missing case; log and snapshot the failing draw
      for offline replay via `tools/pm4_replay.cpp`.
- [ ] **B2.2 Shader corpus expansion** — menu/level shaders may differ from the
      splash corpus.  Capture newly encountered shaders at draw time, add to
      the test corpus (`tests/golden/`), and fix any translator failures.
- [ ] **B2.3 Scalar-evaluator edge cases** — vertex/texture/constant-buffer
      SGPRs encountered during gameplay that the scalar evaluator doesn't
      handle.  Add decode patterns as they appear.
- [x] **B2.4 GDI fallback for in-game** — ensure the GDI DIB path renders
      game frames (not just the boot screen) when Vulkan is unavailable.
- [ ] **B2.5 IPC frame sharing for gameplay** — verify the IPC shared-memory
      path passes game frames (not just boot-screen frames) to the UI
      frontend at playable framerate.

---

## B3 — HLE Stubs & Module Gaps Hit During Gameplay

- [ ] **B3.1 Gameplay-exercise stub sweep** — use the auto-bot (I1) to push
      through menus and into gameplay.  Every time an unimplemented stub is
      called (`LogStubCallOnce`), triage it:
      - Return-0 acceptable? → mark as green-lit.
      - Causes crash/logic failure? → implement the real behaviour.
- [ ] **B3.2 New module registrations** — if the game imports functions from a
      module with no HLE file at all, add the skeleton module, register its
      known symbols, and iterate into B3.1.
- [ ] **B3.3 Atomics/ordering correctness** — weak-memory-order patterns in
      game code (e.g. `atomic_signal_fence`, `atomic_thread_fence`,
      `__c11_atomic_compare_exchange_strong` with relaxed ordering) that work
      on real PS5 Zen2 hardware but break on x64 TSO host.  Fix where
      divergence causes hangs or missed progress.

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
- [ ] **I2.2 Regression test suite** — a `ctest` target (or set of targets)
      that runs the full bot-input replay for each title and fails on any
      crash, hang, or regression.  Track pass/fail history in a JSON
      manifest.
- [ ] **I2.3 Crash bundle improvements (S2.3)** — ensure crash bundles include:
      config, boot-status timeline, last N log lines, last rendered frame
      (DIB/VkImage readback), pipeline cache, and the active input replay.
- [ ] **I2.4 Compat report dashboard** — aggregate `--report=<path>` output
      across multiple bot-run iterations into a markdown table showing which
      stage each title reaches (boot / menu / gameplay / crash-at-stage-X).
      Track per-commit so regressions are visible.

---

## I3 — Performance for Playable Framerate

- [ ] **I3.1 Frame timing & pacing** — verify the vblank pump + flip model
      produces consistent 30/60 fps.  Add timing waterfall from
      `tests/frame_timing.cpp` to the bot-run log so frame-time variance is
      visible per session.
- [x] **I3.2 Direct-mapped guest memory** — 1 GB pool at 0x4000000000, sub-allocates for `Memory::Map(hint=0)`. Replaces per-call VirtualAlloc for guest heap allocations.
- [x] **I3.3 Descriptor pool pre-allocation (O2.3)** — Already implemented in vk_draw.cpp: pre-allocated pool with 2048+ storage/image descriptors, recycled per batch via RotateBatch/ResetDescriptorPool.
- [x] **I3.4 Guest thread scheduling (O3.2)** — `g_hle_mutex` changed from `std::mutex` to `std::shared_mutex`. Dispatch uses `std::shared_lock` (concurrent reads); registration uses exclusive locks.
- [x] **I3.5 VRR frame pacing (R1.3)** — Already implemented: vblank pump checks `g_vrr_active` and uses condition-variable notification instead of 60 Hz timer.
- [x] **I3.6 Shader warmup (O5.2)** — Already handled by persistent VkPipelineCache (O2.2). Pipelines survive between runs via disk cache at `Cache/Pipelines/pipeline_cache.bin`.

---

## I4 — Video Decoder for In-Game Cutscenes

- [ ] **I4.1 Bink2 decoder (V2.1)** — wire `Bink2Decoder` against
      `bink2w64.dll`.  Ship the decoder DLL integration so games using Bink2
      video can play cutscenes.
- [ ] **I4.2 CRI USM decoder (V3.1)** — implement `CriUsmDecoder` using a
      system H.264/H.265 decoder (MFX or FFmpeg).
- [ ] **I4.3 GPU texture upload + overlay (V5.x)** — pipe decoded video frames
      into Vulkan texture upload and composite over the game framebuffer at
      the correct Z-order.  A/V sync via the guest clock.
- [ ] **I4.4 FFmpeg fallback (V4.1)** — `FFmpegDecoder` for MP4/WebM movies
      when the game has a generic video fallback.

---

## I5 — Audio Polish for Gameplay

- [x] **I5.1 Audio timing correctness** — Already correct via kMaxBuffersInFlight=8 blocking queue. Silent null backend paces via PaceSilence wall-clock timing.
- [x] **I5.2 Shared ring buffer (O4.1)** — single lock-free ring across all
      audio backends eliminates per-backend lock contention during
      high-throughput game audio.
- [ ] **I5.3 Zero-copy guest audio (O4.2)** — avoid the extra heap copy between
      the guest audio buffer and the host playback buffer.  Map guest memory
      directly for the audio backend where possible.

---

## I6 — Diagnostics for Fast Iteration

- [x] **I6.1 Boot-status timeline in crash bundle** — the `SetBootStatus` calls
      already trace the boot path.  Export the full timeline (stage + time)
      on crash so we can see the last milestone reached.
- [x] **I6.2 Guest-stub heat map** — `--report` already exports stub-call counts.
      Add a per-run heat map (top-N most-frequently-called stubs) so the next
      target for real implementation is data-driven.
- [ ] **I6.3 Golden frame capture on menu-screen** — once B1 lands, capture a
      reference frame of the title/menu screen via the existing PM4 capture
      tool (`tools/pm4_replay.cpp`).  Future runs compare against this golden
      frame to detect regressions.
- [ ] **I6.4 Snapshot-on-hang** — when the hang detector (I2.1) fires, take a
      full diagnostic snapshot: guest register state, call stack via the VEH
      context, open file handles, thread list, and the last N flips.

---

## Milestone Tracking

| Milestone | Target | How to verify |
|-----------|--------|---------------|
| Boot → splash | ✅ VERIFIED 2026-07-26 | Runs crash-free 45+ s with draws + flips at 2160x1080. Memcpy AV in AGC patch chain fixed via memory validation. |
| Boot → menu | Game-dependent | Menu screen renders with correct GPU output, responds to controller inputs |
| Boot → gameplay | Game-dependent | Player can move, interact, game logic progresses past character-select/title |
| Playable | Game-dependent | 30+ fps sustained, no crashes in first 15 min of bot-run gameplay, audio works, cutscenes play |
| Regression-safe | Ongoing | All bot replays pass in CI; golden-frame comparison detects GPU regressions |
