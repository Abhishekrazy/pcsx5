# PCSX5 Tasks

Progress tracker. Updated the moment a task completes and whenever new work is
discovered. Each entry says what the task is, why it matters, the evidence
behind it, and what *done* requires. History that no longer guides work was
folded into one-line records in "Done" on 2026-09-06 (the detail lives in the
git log, `docs/audits/` and `docs/walkthroughs/`).

Status: `[ ]` open · `[~]` in progress · `[x]` done · `[!]` blocked · `[-]` dropped or falsified

Priority is `(silent-failure risk × blast radius)`, then whether the work makes
later work cheaper. Broad fixes before single-title work.

Last release: **v0.1.2** (2026-09-06): CI zip + Inno installer, Squirrel
updater packages uploaded by hand (see the packaging item).

---

Completed and partially completed entries live in
[docs/COMPLETED_TASKS.md](docs/COMPLETED_TASKS.md). Falsified entries stay
here on purpose: they record what was disproved and are what stops a future
session re-running a dead end (Rule 13).

## Open - shell and launcher

- [ ] **Input ownership while a game is embedded - decision needed.** The
  shell's pad tick keeps reading the DualSense through its in-process core
  while the out-of-process core reads the same device: two readers on one HID
  stream, the interleaving ADR-001 exists to end. Consequence today: Esc/PS
  cannot open the pause menu over an embedded game (the child owns the
  keyboard; verified in SHELL_20260906_055548 and _055749). Recommended, and
  consistent with ADR-001: the core owns all input while a game runs, the shell
  pauses its reader, the core reports "menu requested" through a spare word in
  the IPC block, and the shell drives pause/resume over the existing pipe.
  Alternative: the shell forwards keyboard as pad input over the map. Waiting
  on the user's choice before changing ownership.

- [ ] **Stuck notice is not visible over the embedded game.** Detection is
  VERIFIED (core ticks the IPC frame counter; shell raises FrameStalled after
  6 s without a frame; SHELL_20260906_063100). The notice is hosted in a
  top-level OverlayWindow because the native child paints over WPF
  (airspace), but a full-screen grab mid-stall did not show it. Own iteration:
  DirectComposition/HwndHost sibling, or draw the notice inside the core's
  window.

- [!] **Quick settings popup in-game - SOFT BOUNDARY.** The IPC pipe carries
  only STOP / KILL / PAUSE / RESUME, so a mid-game settings popup could apply
  nothing. Needs an IPC "set option" command with the core applying
  runtime-safe settings (volume, frame limit, VSync, resolution scale) and a
  reply. After the input-ownership decision above.

- [ ] **Two launch implementations in `GameSession.cs`.** `GameThreadProc`
  runs the core in-process and is dead; `Launch` spawns `pcsx5_cli.exe` over
  IPC and is the real path. Boot phases now come from the core's log markers
  (done), but the dead implementation and its phase code remain. Done means:
  one launch path, the dead one removed with its tests adjusted.

- [ ] **WASAPI init fails on a 44.1 kHz output device and silently drops to
  waveOut.** Log: `WASAPI mix is 44100 Hz/2 ch (port wants 48000 Hz stereo)`
  -> `WASAPI init failed; falling back to waveOut`. Sound works through a path
  the user did not choose. Done means: resample to the device mix rate, or
  show the downgrade where a user can see it.

- [ ] **Audio backend default disagrees between config trees.**
  `src/config/config.h:72` defaults `backend = 0` (Off); the repo-root
  `pcsx5_config/global.json` says 0, `dist/pcsx5_config/global.json` says 1,
  `.work/dbg_config/` a third value; `CoreBridge.Pcsx5Options` has no audio
  field. Which the user gets depends on where the binary lives. Done means:
  one default, and the shell can set audio explicitly.

- [ ] **4.12 Keyboard and mouse as a complete no-controller path.** Guest
  side: a GLFW keyboard backend maps 25 keys and bindings can capture keys,
  but there is no mouse input at all (no mouse-as-stick/touchpad/buttons) and
  key-driven analog sticks are unverified. Shell side: mouse works; keyboard
  reachability of every control has not been audited. Verify each half with
  the controller physically disconnected.

- [ ] **Hand verification the harness cannot do** (it cannot press pad
  buttons). Each is INFERRED until the user confirms:
  - 3D pad tilt direction per axis (both signs flipped 2026-09-06 on the
    user's report of a mirrored pad; a single-axis error is physically
    impossible for an IMU, so either both are right now or both wrong).
  - Player-LED pattern (P1 centre, P2 outer pair, ...) and the mute LED's
    local toggle.
  - Spatial navigation on the bindings grid and the Settings option pages;
    Settings side-column reachability; crash-overlay button mapping.
  - 4.3 mic-mute / USB data / USB power status bits (0x35 byte: 0x04, 0x08,
    0x10) - press mute and plug a cable while the Input popup is open.
  - 4.4 two physical pads: indices stay put when one is unplugged.

- [ ] **Sensor scale is UNKNOWN.** `Sample.accel/gyro` are raw int16 counts
  (accel ~8192/g INFERRED from the resting magnitude). The graphs autoscale
  and the 3D pad works unit-free. Establishing counts-per-g and per-deg/s
  needs a measured rotation, not a copied constant.

- [ ] **Remaining artboards to rebuild:** Error overlay, pause overlay,
  first-run setup. (Pause overlay is on tokens but has never been seen over a
  game; verify once a title reaches gameplay under the shell.)

- [ ] **4.11 UI polish** - screenshot every screen, judge against the console
  look, fix spacing/hierarchy per change. Known items: `about.line3/4`
  locale strings still describe the ImGui shell (unused, but wrong); shelf's
  fifth tile clips at 1200 px (fits at fullscreen); the harness window is
  1200x780 while the shell starts fullscreen, so captures show narrower
  layouts than users see.

- [ ] **UI ratchet backlog** (tests/ui_baseline.json): hardcoded XAML
  strings and controls without `AutomationProperties.Name`. The rule is that
  no change adds to it; paying it down is a separate pass.

- [ ] **The stick deadzone setting reaches the shell but not the guest.**
  `input.deadzone` is now applied by the shell to its own stick use (radial,
  rescaled: navigation and the colour picker; 2026-09-07, user's pad drifts).
  The core does not read it: `libpad.cpp` reports a fixed deadzone of 30 in
  the pad info and passes raw stick values to the guest. Done means: the core
  applies the configured deadzone (or reports it so the guest does) with a
  test, so the setting means the same thing in a game as in the shell.

- [ ] **Compat refresh from the database erases the local evidence.**
  `RefreshCompatFromDatabase` rewrites `compat_seed/titles/<id>.json` with
  `source=database` and drops the `evidence` sentence the curated record
  carried (seen as an unstaged diff on PPSA02929.json, 2026-09-06; reverted).
  Done means: the database tier updates the status only, and the local
  evidence text survives.

- [ ] Small: committed build outputs under `src/ui_csharp/bin/**` are still
  tracked; `tools/dream_tool.py` targets the wrong title's eboot;
  `GetWindowRect` includes the ~7 px DWM border (use
  `DWMWA_EXTENDED_FRAME_BOUNDS` on the fallback path); `CheckRebindInput` is
  dead code; `ControllerVisualizer.xaml.cs` (2D pad) is now only a fallback.

## Open - core

- [ ] **`Memory::ReadBuffer` discards `GuardedRead`'s result at 38 sites**
  (`kernel.cpp` 13, `vk_draw.cpp` 6, `elf.cpp` 5, `libkernel.cpp` 3,
  `libatrac9.cpp` 3, `guest_printf.cpp` 2, six singles). A failed read hands
  our own code plausible garbage. `WriteBuffer` was retired the same way
  (the compiler enumerated every site); extending that to `ReadBuffer` is the
  user's call. Start with `elf.cpp` headers and `vk_draw.cpp` vertex data.

- [ ] **A texture is cached by its descriptor, never by its contents**
  (`TextureIdentity`, `src/gpu/vk_draw.cpp`). In-place texture updates show
  their first frame forever; only capacity (>512) evicts. Both reference
  emulators track dirty pages. First confirm a title rewrites a texture in
  place (hash small textures as a probe), then choose content hashing vs
  write-tracking by measurement.

- [ ] **The stub-classification regime is inert.** `RegisterStubContract`
  has no production callers, so `GetStubContract` is always UNKNOWN. The
  import report now exists (165 stubs for PPSA02929, call-count heat map);
  populating contracts is Rule 04 recovery work per symbol, in call-count
  order.

- [ ] **A way to drive the PM4 walker without a GPU.** Every walker test
  reaches real GPU work through `sceAgcDriverSubmitDcb` and segfaults
  `hle_agc_tests`; four workarounds crashed. Blocks tests for the index-offset
  fix and every other walker behaviour. Possibly a robustness defect (walking
  a command buffer with no device should not crash), not only a test gap.

- [ ] **Skipped AGC packets:** NOP `0x19` (DMA data), `IT_EVENT_WRITE`
  (`0x46`), op `0x76` - execute or record why skipping is correct.

- [ ] **RECTLIST mapped to a triangle strip** (`PrimitiveTopologyFromVgt`,
  VGT `0x11`). Each rectangle renders as one triangle. Not hit by PPSA02929
  (all TRILIST, measured); waiting for a title that uses it.

- [ ] **Colour-target registers are not decoded** (`DecodeRenderTarget`
  never sees CB_COLOR0), so `pending_targetless` exists as a fallback. Both
  references decode the full `ColorBuffer` register set. For PPSA02929 the
  guest genuinely never binds a target (measured: 54 registers set, none of
  them colour-target), so this is latent until a title does.

- [!] **FALSIFIED: untargeted *storage* draws were the cause of PPSA02929's
  frozen frame.** Measured 2026-09-07: the title has zero storage bindings and
  its colour-buffer registers are all zero. The discarded draws were ordinary
  sampled sprite draws. Kept so the storage branch is not re-proposed as this
  title's fix.

- [ ] **Progression classification on this machine is noisy.** Measured back
  to back in one session: pre-fix 2/6 runs `progressing`, post-fix 9/12. The
  earlier 6/6 baselines were taken in a more favourable machine state, so
  batch-to-batch comparisons across sessions are unreliable. Either widen the
  sample or make the classifier robust before using progression rate as
  evidence again.

- [ ] **The presentation loop has no isolated test.** Its state machine needs
  a live device and swapchain, and the extractable parts are too trivial to
  test meaningfully. The regression mechanism is a validation run:
  `VK_LOADER_LAYERS_ENABLE=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1`
  with zero `Validation Error` lines as the pass condition. Worth wiring into
  the runner so it is checked rather than remembered.

- [!] **FALSIFIED: the logo doubling comes from combining multiple displaced
  samples.** The shader takes exactly **one** tap. Doubling cannot come from
  multi-sampling here. A large-amplitude single-tap displacement can map two
  screen regions onto overlapping source areas, which reads as doubling.

- [!] **FALSIFIED: the warp is frozen because its time input never advances.**
  Block B index 12 advances monotonically, so the animation input is live.
  The earlier reading that "all eighteen constants are identical at frames
  0, 1, 2, 3 and 60" sampled only Block A, which is genuinely static because
  it is not a displacement table.

- [ ] **Block A is a perspective projection matrix, not displacement
  constants.** `2.4142` is cot(22.5 deg), the standard y-scale for a 45-degree
  vertical field of view; `1.358` is `2.4142 / (16/9)`, the matching x-scale
  for a 16:9 aspect; `-1.002` and `-2.002` are the near/far depth terms of a
  standard projection. `INFERRED` from the numerical identities, not from
  guest source. If correct, `ps 0x215795900` is a projected pass rather than a
  screen-space displacement, and the visible "warp" is a perspective effect
  the game intends. This materially weakens the case that the warp is a
  defect. Confirm or refute by checking whether the pass's vertex shader
  consumes Block A as a matrix.

- [ ] **ROOT CAUSE FOUND: the frame-rate collapse is per-draw re-copying of
  guest storage ranges.** The emulator copies every bound storage range out of
  guest memory on every draw. At about 14 s into a run of PPSA02929 the mean
  bound range grows about 5x, from 105 KB to roughly 530 KB, with the largest
  range going from 256 KB to 1,048,560 bytes; the `FPS:` series starts falling
  at exactly that moment and settles near 1 fps. That is roughly 53 MB/s of
  copying through `Memory::GuardedRead`, which runs at about 150 MB/s in this
  path. The storage-buffer upload phase is about 85% of all GPU-side draw time
  (22.8 s of 26.9 s over 800 draws). Presentation is not implicated: flip
  submission is 0.3 ms and the present stage averages 0.23 ms. Classified
  `SOFT BOUNDARY`. Audit:
  `docs/audits/AUDIT-2026-09-08-frame-rate-collapse.md`.
  Done requires PPSA02929 to hold a frame rate within an order of magnitude of
  60 for a full 60 s run, measured on the `FPS:` line.

- [!] **CORRECTION: the storage ranges do not rotate addresses.** An earlier
  entry said the guest rebuilds its buffers at a fresh address every frame and
  used that to explain why an address-keyed cache cannot hit. That observation
  came from the AGC constant block in `libagc.cpp` and does not describe the
  storage bindings: only **155 distinct bases** appear across a 60 s run.

- [ ] **DEFECT: a guarded write into a write-tracked range is silently
  discarded.** `Memory::TrackGuestWrites` arms a range with `PAGE_READONLY` and
  relies on the hardware fault to disarm it. The guarded primitives instead
  pre-check `IsWritable` on the host side, see the armed page as unwritable,
  and drop the write - `CommitOnFault` returns early for an already-committed
  page and never recovers an armed one. Six call sites in `memory.cpp` behave
  this way. Evidence: tracking the storage ranges produced 5161 dropped writes
  per range in one run (`PPSA02929_20260908_014553`), against zero in every
  untracked run. This is **not** specific to that experiment - the texture
  ranges are tracked today, and the only reason it has not bitten is that
  nothing writes to them through the guarded path. A fix was written and
  measured (a `ReleaseTrackedWriteAt` helper called at all six sites, taking
  dropped writes to 0) but reverted with the experiment, because with tracking
  removed it is unexercised. Done requires the fix plus a test in
  `tests/guest_memory_access_tests.cpp` that writes into a tracked range and
  asserts the bytes land and the generation advances.

- [!] **FALSIFIED: intra-batch reuse captures the redundant copying.**
  Implemented and measured: reusing a buffer already uploaded within the same
  batch gave about 1.4 fps against a 1.0 fps baseline. The redundancy is
  overwhelmingly across frames, not within one, so only a cross-frame cache can
  capture it. This also corrects the 98%-duplicate measurement, whose counter
  was never cleared per batch and therefore measured the whole run.

- [!] **FALSIFIED: write-tracking the storage ranges is a usable fix.**
  Adding `TrackGuestWrites` to the storage path took PPSA02929 from 0.9 fps to
  a flat ~100 fps for a full 60 s run - about 110x, with the collapse entirely
  gone - and broke the title. With the dropped-write defect above also fixed,
  the frame rate rose to a flat 127 fps and the title still froze on the
  Ratalaika splash for 48.6 s while drawing at 125 draws/s. `INFERRED` cause:
  the mechanism was built for a handful of texture ranges and does not survive
  155 large, mutually overlapping, actively rewritten ones - ranges are keyed
  by base so overlapping ones arm and disarm each other's pages, and
  `DisarmWriteRangeLocked` restores `PAGE_READWRITE` rather than the range's
  original protection. Reverted.

- [ ] **RISK carried by the import: no invalidation hook.** An imported window
  is validated once, on first import. If guest memory is released while a
  buffer is still bound to it, the device would read freed address space. Not
  observed in any run, but nothing prevents it. Done requires either a release
  notification from the memory subsystem that retires affected windows, or a
  cheap validity check that does not walk pages (the per-bind `IsReadable` walk
  was measured to cost more than the copy it replaced).

- [!] **FALSIFIED: SharpEmu shadow-compare helps us.** SharpEmu skips a storage
  upload when the guest bytes equal a shadow copy. Implemented and measured at
  **2.4 fps against 4.6** - worse. With the import already carrying 84% of
  binds the copy path runs rarely, so the extra staging copy, compare and
  shadow assignment cost more than the device writes they avoid. Right
  technique for SharpEmu, wrong for us once the import changed which path is
  hot. Reverted.

- [ ] **Runs classify `frozen` about half the time, 20-22 of 29 unique
  frames.** Appeared alongside the dispatch fix, but the classifier itself
  reports the status was not stable across baseline samples, and the title now
  reaches its title screen where before it did not. `UNKNOWN` whether this is
  the game sitting on a static screen or a real stall. Settle it before reading
  it either way - do not report it as either a regression or progress until
  measured.

- [!] **CORRECTION: the "157 microseconds per HLE dispatch" figure was a
  measurement artifact.** The timer incremented two shared atomics inside every
  call, so on a path taken 2.26 million times a minute across several threads it
  largely measured its own contention - re-running it on the fixed build still
  reported 137-220 microseconds. The dispatch fix stands on the end-to-end frame
  rate (2.9-4.7 wide, to 4.3-4.7 tight); the absolute per-call cost is
  `UNKNOWN`. Measuring it needs per-thread counters aggregated at exit.

- [!] **FALSIFIED: something accumulates over a run and causes the frame drop.**
  Tested directly. GPU pool sizes over a 120 s run are flat and small (retired 0,
  imported 14-16, guest buffers 21-39, textures 4-5), and the frame rate peaks at
  109, drops once, then holds 4.2-4.6 for the rest of the run with no progressive
  decay. The drop is the already-characterised transition where the guest bound
  ranges grow about 5x. The remaining cost is a steady state, not a leak.

- [!] **FALSIFIED: draw-call count is the bottleneck.** The title issues only
  **14-18 draw calls per frame** in a command buffer of about 1000 dwords. The
  count is trivial; the cost is per draw - about **5.3 ms each** inside the
  command-buffer walk.

- [ ] **NEXT: texture upload costs about 2.6 ms per draw** (3.17 s over 1200
  draws), now the largest GPU-side item and roughly half the per-draw cost. The
  texture path already has a write-generation skip and its ranges *are* tracked
  via `TrackGuestWrites`, so establish first why it is not skipping - either the
  generation genuinely moves every frame, or the skip is not reached. Do not
  assume; measure the hit rate before changing anything.

- [ ] **Attribute the 63% of frame time spent outside the command-buffer walk.**
  Per-frame split with both fixes in place: 260 ms wall, 95 ms walk. Needs a
  non-contending instrument (see the correction above).

- [!] **RPCS3 text rendering does not apply here.** RPCS3 text rendering is its
  own overlay UI - trophy popups, home menu, on-screen keyboard - drawn on top of
  the game. PCSX5 does not render this title's text at all: the words are a guest
  texture sampled and displaced by the guest's own pixel shader `0x215795900`,
  and the artifacts are in that pass's output. There is no text-rendering code
  here to replace. RPCS3 is GPL-2.0 so it is licence-compatible if something else
  there proves useful.

- [!] **FALSIFIED: scalar evaluation can be memoized on the user-data bank.**
  Shader scalar evaluation is **75% of remaining draw time** - over 2400 draws:
  decode 62 ms, evaluation **7,621 ms**, descriptor setup 54 ms, Vulkan execute
  2,452 ms, i.e. 3.2 ms per draw and rising. Memoizing it on (shader address,
  user SGPR bank) raised the frame rate 4.6 to 6.9 fps **and cost unique frames,
  20 down to 16** - isolated by disabling only the cache lookup and re-running.
  The guest rewrites its descriptor table *in place*, so the table contents
  change while the user data pointing at it does not: the key never moves and
  the cache serves stale bindings. Reverted rather than trade correctness for
  frame rate.

- [ ] **NEXT: split scalar evaluation into locate and read.** The largest
  remaining measured cost (3.2 ms per draw, 75% of draw time). The part that
  *locates* descriptors is a pure function of the shader and the user-data bank
  and can be cached; the part that *reads* their current values must run per
  draw. That requires a change inside `GcnEvaluateScalarState` and is its own
  iteration. Done requires the frame rate to improve with unique-frame count
  held at or above the current 20 of 29.

- [ ] **Five of the six titles under `Games/` crash during boot**, with four
  distinct signatures, so there is no single foundational fix:
  PPSA10264 `0xC0000005` at RIP `0x802e850c0`; PPSA20591 `0xC0000005` at
  `0x801399fa0`; PPSA15552 `0xC0000005` at `0x81000b87f`; PPSA10112
  **`0xC000001D` (illegal instruction)** at `0x800a2bd0c`; PPSA01668 jumps to
  **RIP 0** with `libkernel::Q3VBxCXhUHs#T#T` (`memcpy`) as the last HLE call.
  Each needs its own investigation. Break out per title before starting.

- [!] **FALSIFIED: module relocation explains the other titles' crashes.** All
  five relocate (`MODULE_RELOCATE preferred=0x800000000 allocated=0x810000000
  reason=collision`), which looks like a shared cause - but **PPSA02929
  relocates identically and works**. Relocation is normal for these PIE
  modules: the eboot maps at `0x800000000` and dependent modules then collide.
  Recorded so the obvious conclusion is not re-drawn.

- [!] **FALSIFIED: the scalar evaluator symbolic path walk is expensive.**
  Instrumented: **1 path and about 62 instruction visits per evaluation**, 0%
  skipped. 62 instructions cannot cost 3.2 ms; the cost was the per-dword memory
  guard above.

- [!] **FALSIFIED: the per-binding `LOG_INFO` dumps in `AgcEvaluateDrawShader`
  are the cost.** Made one-shot per shader; log volume stayed at 27-28k lines and
  the frame rate stayed inside run-to-run variance. Reverted.

- [ ] **`Memory::IsWritable` has the same structure as `IsReadable`** - per-page
  `Query`, so a `VirtualQuery` syscall plus the global region lock per page - and
  was **not** changed. It sits on every guarded write path. Measure it before
  assuming it matters, but the structure is identical to a defect that measured
  at 95 microseconds per call.

- [ ] **The "top-region blocky artifact" is a displaced second copy of the logo,
  not noise.** At 9.3 fps the title screen shows the logo twice: centred, and
  again in the upper band where the blocks were recorded. `OBSERVED`
  (`PPSA02929_20260908_104239` frame 28). This merges two tracked artifacts into
  one: the blocks and the warp are the same phenomenon, consistent with the
  shader audit prediction that a large-amplitude single-tap displacement can map
  two screen regions onto overlapping source areas. The remaining visual question
  is the displacement amplitude in `ps 0x215795900`.

- [ ] **`Query` reports pool memory as writable regardless of real page
  protection.** For addresses inside the direct-mapped pool, `Query` answers
  from the region table and never consults `VirtualQuery`, so an armed
  (PAGE_READONLY) page still reads as writable. Not wrong by design - the
  region table is documented as the sole authority for pool allocations - but
  page protection and reported protection can disagree there, and it is why
  `TestGuardedWriteIntoTrackedRange` must set `PCSX5_DISABLE_POOL=1` to
  reproduce the defect it guards. Decide whether this divergence is intended.

- [ ] **A ghost of the title logo persists over the gameplay background.**
  Stale render-target content not cleared between scenes; visible as a faint
  "Dreaming Sarah" behind the playable scene. This is the mission brief's
  "motion blur" item, now with a specific characterization. `OBSERVED`.

- [!] **FALSIFIED: missing fog / transparency artifacts are caused by blending
  being disabled.** Measured the actual `CB_BLEND0_CONTROL` values reaching the
  pipeline: seven distinct states, six with `enable=1` and correct
  `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA` factors (`0x65040504`, `0x65000500`,
  `0x61010101`, ...). The decode also matches the GFX10 register layout field
  for field. Blending is not disabled.

- [!] **FALSIFIED: missing fog is caused by draws being rejected.** Instrumented
  every `VkDrawExecute` outcome over a 90 s run: **zero** draws dropped. Nothing
  is being refused by the backend, so the fog layer is either never submitted by
  the guest or is submitted and renders invisibly.

- [!] **FALSIFIED: texture filtering is wrong.** The guest requests point
  filtering (`XY_MAG/MIN_FILTER` = 0 in sampler word 2) and the decode produces
  `VK_FILTER_NEAREST`, which is correct for this title's pixel art. The smearing
  on the glyphs is therefore not a filtering choice.

- [ ] **DEFECT: the sampler cache key omits word 3.** `EnsureSampler` hashes
  `w[0]`, `w[1]` and `w[2]` but not `w[3]`, so two samplers differing only in the
  final descriptor word collide and the first one wins. Not demonstrated to
  affect this title - both observed samplers differ in earlier words - but it is
  a real cache-correctness bug. Done requires `w[3]` in the hash and a test.

## Requested features, not yet started

- [!] **HARD BOUNDARY: Android is not reachable from this architecture.**
  Asked 2026-09-08. The blocker is the CPU, not the UI, and no amount of
  Avalonia work moves it.

  PCSX5 runs guest code by **direct execution**: the PS5 is x86-64 and so is the
  host, so guest instructions are executed natively and there is no translation
  layer at all (`src/cpu/cpu.h`: "we use direct execution - guest code runs
  natively without translation... No JIT compiler is needed"). Verified: a
  search for any recompiler, dynarec or block-translation code in `src/cpu/`
  returns **nothing**.

  Android is overwhelmingly ARM64. Running an x86-64 guest there requires a
  full x86-64 to ARM64 recompiler - the single largest subsystem in an emulator
  of this kind, and one this project has never needed and does not have. That is
  a `HARD BOUNDARY` under Rule 09: it is not a bounded porting task, it is a new
  core.

  Secondary blockers, all of which would also have to be solved even on an
  x86-64 Android device: 127 Win32 API call sites, 40 `__try` blocks, 7 public
  headers including `<windows.h>`, and `src/hle/dispatcher.asm` - hand-written
  x86-64 assembly using the **Windows** x64 calling convention. Only the GPU
  path is portable: Vulkan is already the backend and Android speaks Vulkan.

  **Why the PCSX2 comparison does not transfer** (asked 2026-09-08). The PS2's
  Emotion Engine is a **MIPS** CPU. A MIPS guest cannot run natively on any host
  PCSX2 targets, so PCSX2 has needed a recompiler since its first release -
  MIPS to x86 - and that recompiler is core infrastructure, not an add-on.
  Reaching ARM64 therefore meant writing a **new code-generation backend for a
  recompiler that already existed**: substantial, but bounded, and built on
  machinery that was there from day one. The same is true of any emulator whose
  guest architecture differs from its host.

  PCSX5 has the opposite shape. The guest is x86-64 and so is the host, so there
  is no translation layer to add a backend to - there is no translation layer at
  all. Going to ARM means building the whole thing from nothing, which is a new
  core rather than a port.

  This is a family trait of PS4/PS5 emulators, not a PCSX5 shortcut. Verified
  against the two reference clones on this machine:

  - **Kyty**: the only component named "recompiler" sits under its graphics
    shader tree and is a **GCN shader to SPIR-V** recompiler. There is no CPU
    recompiler anywhere in that project.
  - **SharpEmu**: its entire CPU layer is `Cpu/Native/DirectExecutionBackend.*`,
    including a file literally named `DirectExecutionBackend.Amd64Compat.cs`.
    There is no interpreter and no dynarec; a search for `Interpreter` under
    `Cpu/` returns nothing.

  So all three PS4/PS5 emulators examined - PCSX5, Kyty and SharpEmu - execute
  guest code directly on an x86-64 host, and none of them can reach ARM without
  the recompiler none of them has.

  Recorded so it is not re-proposed as a UI or packaging task. Revisit only if a
  guest recompiler is ever specified as its own multi-quarter project.

- [ ] **Avalonia port will not by itself produce a multi-platform release.**
  Worth stating next to the port task because the two are easy to conflate. The
  cross-platform audit already found that "the core is the gate, not the shell -
  a portable shell on top of a Windows-only core ships nothing"
  (`docs/audits/AUDIT-2026-09-07-cross-platform-feasibility.md`). Avalonia
  replaces WPF, which removes the *shell's* Windows dependency; the core keeps
  every one of the couplings listed above. Doing the shell first is defensible
  as de-risking, but it must not be scheduled as "ship Linux/macOS".

  The audit's step 1 - removing `<windows.h>` from the 7 public headers behind a
  small platform header - is low risk, changes no behaviour, and unblocks
  everything after it. That is the cheapest real progress toward a portable core
  and is independent of the UI work, so the two can proceed in parallel.

- [ ] **UI debt has grown past what Rule 12 records.** `MainWindow.xaml.cs` is
  now **6,773 lines** (the rule documents ~4,700) and `MainWindow.xaml` is
  **2,405** (documented ~1,900), across 48 C# files and 4 XAML files with no
  MVVM layer. This matters for the Avalonia port specifically: the port cost is
  roughly proportional to the code-behind, and `System.Windows*` is imported in
  well over a hundred places. Extracting focused classes out of the code-behind
  before the port would make the port smaller, and is useful even if the port
  never happens. Update the measured figures in Rule 12 when this is addressed.



Recorded together because they arrived as one request; each is its own change.

- [ ] **Double-click the output window to toggle fullscreen.** The mechanism
  exists (`ToggleFullscreen`); this needs a GLFW mouse-button callback with
  double-click detection. Small.

- [ ] **In-game overlay: frame graph and performance matrix on a hotkey.**
  `Diagnostics::RenderTimingOverlay` already exists, with a timing waterfall and
  rolling FPS graph written - but it is **never called** and is compiled out
  behind `#ifdef IMGUI_VERSION`. ImGui is fetched by CMake and then **never
  compiled or linked into any target**; there is no `imgui_impl_vulkan` or
  `imgui_impl_glfw` anywhere. So this needs real integration: compiling ImGui,
  adding the GLFW and Vulkan backends, a descriptor pool, and a render-pass
  hook. The window-title readout is the current stand-in and is invisible in
  fullscreen, which is exactly why the overlay matters. Largest item here.

- [ ] **Input profiles: configurable keyboard mapping, switchable.** The
  keyboard mapping is currently a hardcoded `switch` in
  `src/gpu/vulkan_backend.cpp`. Needs a profile model in config, a UI, and the
  mapping moved out of the switch.

- [ ] **Multiple controllers / up to 8 players with a tabbed interface.**
  `scePadOpen` currently serves a single primary handle (`IsPrimaryPadHandle`).
  Multi-pad support is an HLE contract change (per-handle state, per-user ids)
  plus a UI. Recover the contract before implementing (Rule 04).

- [ ] **Autosave not working.** Not yet investigated. Establish first whether
  the guest calls the savedata API at all and what it receives - `libSceSaveData`
  and `libSceSaveDataDialog` both resolve to HLE stubs today
  (`ModuleGraph: 20 missing dependencies`).

- [ ] **CLI run shows no logo on the taskbar.** The GLFW window has no icon set;
  needs `glfwSetWindowIcon` with the app icon. Small.

- [ ] **Fog layer missing versus a reference capture.** A reference screenshot of
  another emulator running the same scene shows a fog overlay that our render
  does not draw. Blending and draw rejection are both excluded above, so the
  next step is to compare the draw list for that scene against what the guest
  submits, not to change the blend path.

- [ ] **NEXT BOUNDARY: re-take the FRAMECOST split.** After the
  import, GPU-side draw work is about 3.4 s of a 60 s run (texture 3.17 s,
  storage 0.20 s, pipeline 0.036 s, recording 0.030 s) - roughly 6% of wall
  clock. The other 94% is unattributed. Profile the command-buffer walk and the
  guest side before touching the GPU again. Texture upload is now the largest
  GPU-side item and the obvious candidate for the same treatment.

- [ ] **Adopt Kyty page-granular dirty tracking for the ranges that cannot be
  imported.** 16% of binds and 28% of bytes still copy. Kyty refcounts read and
  write watchers per 4 KiB page behind a vectored exception handler and uploads
  only dirty sub-ranges, with a guest-address-keyed cache, LRU and a GC; it also
  flushes GPU-written ranges back to guest memory on a read fault. That is the
  mature form of the mechanism whose immature form broke the title (see the
  falsified write-tracking entry above). Neither Kyty nor SharpEmu imports host
  memory, so this is the technique both rely on. Larger than one iteration;
  break down before starting.

- [ ] **Checkerboard artifact on the title text.** A one-pixel checkerboard
  across the "Dreaming Sarah" glyphs, in the same pass already localized as the
  source of the warp (`ps 0x215795900`). Two untested candidates: the sampler
  addressing mode, which `AUDIT-2026-09-07-artifact-localization.md` named as
  the leading unaudited suspect, or a dithered alpha fade that reads as a
  checkerboard only because the frame rate is low enough to see single frames.
  `UNKNOWN`. The second is cheap to discriminate: it should visually blend as
  the frame rate rises.

- [ ] **Subtask: decide how to avoid re-copying unchanged storage ranges.**
  Two candidates remain, both larger than one iteration. (a) Repair and
  generalise write tracking as its own change - fixing the dropped-write defect
  first under test, then settling overlapping-range and protection-restore
  semantics; the naive version is measured above and does not work. (b) Import
  guest pages into the device with `VK_EXT_external_memory_host` and bind them
  directly, removing the copy rather than skipping it. This GPU supports the
  extension with `minImportedHostPointerAlignment = 0x1000`, and guest memory
  suits it because `Memory::Translate` is the identity - guest addresses are
  already page-aligned host addresses. Option (b) needs no memory-subsystem
  change and is the better first attempt; its open risk is invalidating imports
  when guest memory is released.

- [!] **FALSIFIED: a write-generation skip for storage buffers fixes the
  collapse.** Implemented and measured: `TryGetGuestWriteGeneration` returns
  false for every one of the 4949 binds in a 60 s run because nothing calls
  `TrackGuestWrites` on these ranges, so the skip never fires and the fps was
  unchanged. Reverted.

- [!] **FALSIFIED: persistent mapping or a storage ring fixes the collapse.**
  Both were implemented and measured. Per-bind `vkMapMemory`/`vkUnmapMemory`
  was replaced with a lifetime mapping, and the 256-entry address-keyed buffer
  cache - which never hits, because the guest rebuilds its constants at a fresh
  address every frame, so it fills, retires wholesale and reallocates - was
  replaced with a bump-allocated ring. Both remove real work and neither
  changed the frame rate, because neither removes any copying. Reverted; the
  designs are recorded in the audit if the copy problem is solved and they
  become worth revisiting.

- [ ] **The guest advances 104 frames in 120 wall seconds (~0.87 fps).**
  Block B index 12 is a fixed-timestep accumulator: `1.7333 / 0.016666` is
  exactly 104 ticks of 1/60 s. So the guest's own frame counter reached 104
  while 120 seconds of wall clock elapsed. This is the same defect already
  tracked as the submission-rate collapse, now with a direct guest-side
  measurement rather than a host-side draw count. It is the dominant runtime
  problem: at under one frame per second every capture is effectively a still,
  which is why an animated effect reads as a frozen distortion. Done requires
  the guest frame rate to be within an order of magnitude of 60.

- [ ] **Audit the sampler addressing mode for the single sample in
  `0x215795900`.** Out-of-range coordinates are the leading remaining
  candidate for the intermittent blocks, and a large displacement can push the
  coordinate outside [0,1] where behaviour depends on the addressing mode.
  Not audited yet.

- [ ] **Two translator rounding nuances, neither demonstrated to matter.**
  `VRcpF32` lowers to an exact `OpFDiv` where hardware is approximate (ours is
  more accurate, not less); `VCvtPkrtzF16F32` specifies round-toward-zero
  while `PackHalf2x16` rounds to nearest-even. Candidate discrepancies,
  recorded so they are not rediscovered.

- [ ] **A strip of blocky artifacts along the top edge of the frame**
  (`PPSA02929_20260907_180248` frame 18, roughly the first 45 rows). Does not
  look stylistic. Newly observed, uninvestigated; cheap to localise with the
  render-target capture technique.

- [!] **UNCLASSIFIED: the title logo distortion.** The source texture is
  provably correct and the words are legible on screen, but warped. There is
  no reference capture of this title PS5 title screen and a web screenshot of
  another build is not a sound basis for pixel comparison. The warp is smooth,
  continuous and animated, which is consistent with the deliberate dream
  effect this game uses but does not prove it. No pipeline change was made on
  the strength of it looking unusual.

- [ ] **PPSA02929 title logo is still distorted on screen** although its
  source texture is now correct: warped and doubled after a three-stage
  post-process chain. This game aesthetic includes a deliberate dream wobble,
  so it may be intended. `UNKNOWN` without a reference capture.

- [ ] **No Vulkan validation layer runs in this configuration.** Zero
  validation output appears in any run log, so all GPU evidence to date is
  behavioural. Running once under the validation layers and classifying what
  they report is worthwhile and not yet done.

- [ ] **PPSA02929's scene renders but is not correct.** Colours and some
  geometry are wrong and the image shows banding and stepped edges
  (`PPSA02929_20260907_123027`). The title is not playable. Next after the
  TLS crash below, which caps run length.

- [!] **FALSIFIED: the submission-rate collapse explains the black screen.**
  The 15x drop (655 then 43 command buffers per 30 s) is real and was measured
  on the fixed build, but the black screen had a different cause entirely -
  the render-target sampling defect above. Whether the collapse itself still
  occurs now that the scene renders has not been re-measured.

- [ ] **`RELEASE_MEM` packets have no consumer in the walker.**
  `sceAgcCbReleaseMem` emits the packet; nothing writes the fence value it
  asks for, so a guest polling that location would wait forever. No current
  title exercises it (PPSA02929 emits none), so a consumer would be untested
  code today. Layout already recovered: control at +8 with data selection in
  bits 16-23, destination at +12/+16, data at +20/+24; selection 1 writes 32
  bits, 2 writes 64, 3 writes a clock sample. Implement with a focused test
  when a title needs it.

- [ ] **Three AGC patch-address entry points are unimplemented stubs**
  (`sceAgcWaitRegMemPatchAddress`, `sceAgcQueueEndOfPipeActionPatchAddress`,
  `sceAgcDmaDataPatchSetDstAddressOrOffset`). They write a real address into a
  previously emitted packet at an opcode-dependent field offset. PPSA02929
  called them before the register-group fix and no longer calls them at all,
  so nothing exercises them today; they become correctness-critical for any
  title that does.

- [ ] **Five AGC entry points stopped being exercised after the register-group
  fix** (`sceAgcDcbWaitRegMem`, `sceAgcDcbDmaData`, `sceAgcCbReleaseMem`,
  `sceAgcCbNop` and the patch-address family): PPSA02929 used 32 `libSceAgc`
  imports before the fix and 27 after. Consistent with the engine taking a
  different setup path once its register groups are identifiable, but
  `UNKNOWN` whether the old path is genuinely unnecessary or the guest now
  fails earlier and never reaches it. Worth settling before trusting the new
  path.

- [ ] **Some host threads reach guest code without a bound thread pointer.**
  Exposed by the fix above, which makes them fall back to the shared TLS block
  rather than crash. A thread that should own private TLS but silently shares
  the main block is a latent correctness problem: `cpu.cpp` notes that worker
  threads aliasing the main fs base "corrupted per-thread runtime caches".
  `UNKNOWN` which threads these are and how they enter. Establishing that
  needs the bind sites correlated against the host thread ids seen executing
  guest code.

- [!] **FALSIFIED: only one of the two display buffers is ever presented.**
  A probe firing every 60th present observed one buffer, but the buffers
  strictly alternate, so it only ever saw one parity. Flips are even at 235
  and 234. Kept because the probe artifact is easy to reproduce.

- [ ] **`sceVideoOutAddFlipEvent` returns success on every repeat call.**
  PPSA02929 calls it once per frame (571 times in 60 s) against the same
  equeue and handle, and we return 0 each time. Retail firmware is unlikely
  to accept an unbounded re-add of the same event; our unconditional success
  may hide an error the guest would handle (Rule 04). `UNKNOWN` - needs the
  caller's use of the return value disassembled. Not the cause of the frozen
  frame.

- [ ] **Dreaming Sarah (PPSA02929) freezes after the splash.** Baseline
  status `frozen` from 3 samples. `FALSIFIED` (2026-09-07): this is not a
  stall. The guest renders 13 draws and flips every frame, and audio runs
  throughout; the frame does not change because most draws are discarded.
  See the storage-binding task above, which now owns this. Kept so the
  "the stall is elsewhere" reading is not re-attempted.

- [ ] **Save-data mount returns an empty mount point** - the guest builds
  `/-saveindex` with no prefix; `sceSaveDataSetParam`/`SaveIcon` are stubs.

- [ ] **A guest-filesystem test fixture** - blocks tests for
  `feof`/`fgets`/`fgetc` (IMPLEMENTED, not VERIFIED) and the guarded-transfer
  reporters.

- [ ] **Wire haptics into the emulator, not just the probe.**
  `PlayHapticsPcmBlocking` blocks for the audio's duration; production needs a
  streaming interface on its own paced thread fed by the guest.

- [!] **DualSense speaker over Bluetooth needs a decision.** Report `0x35`
  carries a 200-byte Opus frame; there is no PCM lane. Writing an encoder was
  rejected (ADR-002); vendoring libopus (BSD-3) is the recommendation, and a
  new dependency is the user's call. Over USB the speaker is a normal Windows
  endpoint needing nothing.

- [ ] **DualSense microphone input over Bluetooth is UNKNOWN** - every source
  covers output only.

- [ ] **`third_party/LibAtrac9` has no README.md** as the vendoring policy
  requires (upstream, pin, licence, compatibility, owner, update procedure).

- [ ] **ADR-001 step 4** (steps 2-3 resolved by 4.9: the shell reads the pad
  through the core). Step 4 is the input-ownership item in the shell section.

- [ ] **Performance: ~9 fps against a reference emulator's 60 fps.** Measure
  honestly first - flip rate is not uniform, so flips/duration compares
  nothing across runs of different lengths.

- [ ] **Cross-platform port (Linux / macOS) is gated on the core, not the
  shell.** Windows coupling sits in memory (`VirtualAlloc` + two VEHs), guest
  TLS at TEB offset `0x1480`, the Win32 `CONTEXT` used as the syscall register
  ABI, 40 `__try` blocks and 8 public headers including `<windows.h>`
  (`docs/audits/AUDIT-2026-09-07-cross-platform-feasibility.md`). If started,
  in this order, each its own change: header hygiene; memory behind a platform
  layer; `sigaction`/`sigaltstack` fault path (subsystem boundary, ask first);
  threads/TLS (macOS fs-base is `UNKNOWN`, possibly a hard boundary);
  periphery; CMake. Shell comes last, and the user has decided it will be
  **Avalonia** (2026-09-07; the framework survey is in the audit). Not
  scheduled yet - the user will say when.

- [ ] **Settings changed while a game is running reach the core only at the
  next launch.** The child core reads `global.json` once in `pcsx5_init`
  (`src/core_api.cpp:216`) and there is no config message on the IPC
  channel. Shell-side values (music, theme, deadzone, rumble) apply live as
  of 2026-09-07; core-side ones (volume, resolution scale, fullscreen)
  need a launch. Decide whether a live "config changed" IPC message is worth
  it before wiring one.

- [ ] **Two audio output backends coexist** (`src/hle/libaudioout.cpp` and
  `src/hle/audio/audio_device.cpp`). Found during the portability inventory.
  Decide the owner, migrate, delete the other (no parallel implementations
  without a removal plan).

- [ ] **XInput is polled from two places** (`src/gpu/vulkan_backend.cpp:109`
  loads `XInputGetState` itself; `src/gpu/input/xinput_backend.cpp` is the
  input backend). One owner for pad polling; the Vulkan backend should not
  read controllers.

- [ ] **`link_libraries(dualsense_windows opus hid)` is global**
  (`CMakeLists.txt:241`), so every later target links HID/Opus. Replace with
  per-target `target_link_libraries` on the object that needs them.

- [ ] **Audio decode is thin** (`libatrac9.cpp` 370 lines, 5 symbols vs
  shadPS4's full AJM with AT9/AAC). LOW: PPSA02929 makes one AJM call per run.

---

Shell, 2026-09-06 (v0.1.1):

Core and harness, 2026-08/09:

---

## Falsified - kept so they are not re-attempted

- [-] "Executing every targetless draw recovers lost work" - the 9-12 draws
  per frame are byte-identical (shaders, texture contents, vertices, offset 0);
  the queue made the screen black and was reverted. The title is frozen, so
  identical draws are a symptom.
- [-] "The colour-target binding is never observed is a defect" - the guest
  sets 54 registers, none colour-target; it composites to scanout.
- [-] "DualSense vibration/lightbar/mute/sticks are broken in the core" -
  all verified working on hardware; the breakage was the shell's duplicate
  reader (since deleted).
- [-] "The accelerometer is misparsed only over Bluetooth" - it was the
  accel/gyro field swap, identical on USB.
- [-] "The shell's config causes the Dreaming Sarah launch crash" - the CLI
  with the shell's exact flags ran clean; the cause was the IPC frame hook.
- [-] "Frames wider than the 1920 IPC cap are skipped" - WriteFrame clamps.
- [-] "The log box ignores a DynamicResource foreground" - ClearType
  fringing read by eye; pixels sampled identical.
- [-] "0xFFFFFC18 condvar timeout is a misread negative" - the reference
  types it unsigned and waits the same ~71 minutes.
- [-] "The half-black quad is a GPU defect" - a wipe transition frozen
  part-way.
- [-] "The guest never calls sceKernelWaitEqueue" - once per frame; only the
  blocking path was counted.
- [-] "End-of-pipe release-mem is never executed, so the guest waits" - its
  data-select is zero on every occurrence; skipping is correct.
- [-] "The title is merely slow" - twenty minutes, no new guest output.
- [-] "Background loading stalled at 49 of 65 files" - the rest are music,
  streamed on demand.
- [-] "`_Getptolower` hot, `9BcDykPmo1I` unresolved" - cached table, and it
  is `__error`; the inventory's bare-NID parsing was the real defect (fixed).
