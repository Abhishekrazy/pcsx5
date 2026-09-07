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

## Open - shell and launcher

- [x] **Release zip was 345 MB; now 78 MB** (2026-09-06). Two causes: the
  zip step packed `dist\*` wholesale, and running the app from dist had left
  88 MB of decoded-audio cache, a 55 MB stray rar, crash dumps, logs, .work
  and the user's own config/favourites/recent-plays in it (a privacy leak as
  much as a size problem); and the self-contained single-file exe was
  published uncompressed (186 MB). Now: `EnableCompressionInSingleFile`
  (exe 78 MB) and the zip is built from an allowlist staged into
  `.work/release_stage` (exe, CLI, core DLL, bink2, README, VERSION, plugins,
  tools, assets, lang; *.log/*.pdb/*.dmp stripped). v0.1.1's asset replaced.
  Remaining bulk is the .NET 9 + WPF runtime, and WinForms assemblies pulled
  in by NAudio (~23 MB raw) - a framework-dependent build would be ~10 MB but
  needs the runtime installed; not changed.

- [x] **Update prompt and manual check** (2026-09-06, user request). At
  startup and from System Information > Check for updates: an overlay
  "PCSX5 X is available, you have Y" with Download & install (Squirrel
  installs: download + apply with a progress bar, then Restart now), or Open
  download page (zip copies, via the GitHub releases API), Skip this version
  (ui.skipped_update_version; only a newer release asks again) and Later.
  `UpdateChecker.cs` owns the checks (Rule 11); pad: Cross/Square/Circle with
  hints.update; 14 keys in eleven locales. Seen:
  artifacts/runtime/SHELL_20260906_232913/frames/frame_0001.png.

- [x] **2D controller art removed** (2026-09-06, user request): the 3D pad
  is the only visualizer, so the VSCView/Gamepad-Asset-Pack sprites, layout
  code (~370 lines of InputTabView), `ControllerVisualizer.xaml` and the
  csproj content are gone; credits now name the 3D model's author
  (AHarmlessPotato, CC-BY-4.0) in the README and the in-app credits string.

- [~] **Release packaging: Squirrel assets are built by hand; make it a
  script.** `Squirrel.exe pack` from the NuGet cache
  (clowd.squirrel/2.11.1/tools) with `--allowUnaware` produces RELEASES, the
  full and delta nupkgs and pcsx5Setup.exe from the release stage; the
  previous full nupkg must be in the release dir for the delta. Done for
  v0.1.2 by hand (this session); a `-Squirrel` switch in build_release.ps1
  is the remaining step. CI (ci.yml) builds the zip and the Inno installer on
  a tag push; its v0.1.1 run failed on a red doc_links test at that commit. v0.1.0 shipped `pcsx5-0.1.0-full.nupkg` and a
  Setup.exe that the in-app `Squirrel.UpdateManager` (MainWindow.xaml.cs:395)
  reads; nothing in the repo produces them (build_and_package.ps1 only knows
  Inno Setup, which is not installed here). Done means: a scripted, repeatable
  way to build the Squirrel release assets, run for v0.1.1, and the release
  updated.

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

- [x] **3D pad renderer swapped to Helix Toolkit (D3D11) for the normal map**
  (2026-09-07, user decision; ADR-004). WPF's Viewport3D has no shader stage,
  so the model's normal and roughness maps were unusable; the pad now renders
  through HelixToolkit.SharpDX.Core.Wpf 2.27.3 (MIT, pinned) with base colour
  + normal map and a plastic specular. Found on the way: Helix's PBR material
  did not sample this model's albedo map (Phong path used instead), and Helix
  drifts when a Transform3D is mutated in place (fresh MatrixTransform3D per
  update). Textures now shipped unbaked plus *_normal and
  *_metallicRoughness at 1024. Seen: artifacts/runtime/SHELL_20260907_023417
  frames/frame_0002.png (embossed PS logo, speaker holes, creases).

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

- [x] **Retained targetless draws are all composited at the flip.** Done
  2026-09-07 (commit 6d3b6ca). The retained-draw slot held one draw, so a 2D
  title that builds each frame from many sprite draws -- none of which binds a
  colour target -- kept only its last sprite per frame. The slot is now a
  queue composited in submission order. Measured on PPSA02929 over 60 s:
  draws executed 273 -> 1107, dropped 1338 -> 400 (83% -> 27% discarded),
  unique frames 5 -> 16; a 25 s run classifies `progressing`. 52/52 ctest.
  Audit: `docs/audits/AUDIT-2026-09-07-targetless-draw-storage-path.md`.

- [x] **Storage image bindings are computed.** Done 2026-09-07 (same commit).
  `is_storage` was never set anywhere in the tree, so every image binding was
  declared sampled and any shader writing an image was mistranslated -- even
  though the Vulkan side already implemented storage usage, layout and
  descriptors. `GcnRequiresStorageImage` supplies it, folded into the shader
  cache key, with tests in `tests/shader_tests.cpp`.

- [!] **FALSIFIED: untargeted *storage* draws were the cause of PPSA02929's
  frozen frame.** Measured 2026-09-07: the title has zero storage bindings and
  its colour-buffer registers are all zero. The discarded draws were ordinary
  sampled sprite draws. Kept so the storage branch is not re-proposed as this
  title's fix.

- [x] **The AGC register-group type identifiers are restored.** Done
  2026-09-07 (commit a25816e). The guest identifies a register-default group
  by an SDK-supplied 32-bit id; we wrote the register space (always 0) there
  instead, under a comment calling them "metadata only". Every group looked
  alike, so the guest never emitted real register offsets for its
  render-target block - which is why this title appeared never to bind a
  colour target and why 83% of its draws were discarded by the targetless
  composite heuristic. Restoring all 149 groups: draws executed 273 -> 1150,
  dropped 1338 -> **0**, deferred composites 206 -> **0**, unique frames
  5 -> 17 of 29. Audit:
  `docs/audits/AUDIT-2026-09-07-black-screen-after-splash.md` section 9.

- [x] **Render-target extent was transposed.** Done 2026-09-07 (same commit).
  `CB_COLOR0_ATTRIB2` packs height low and width high;
  `DecodeRenderTarget` had them swapped, so a 1280x720 surface decoded as
  720x1280. Invisible until targets decoded, because the composite fallback
  sized itself from the display buffer. The existing test encoded the wrong
  order in its fixture and so agreed with the wrong decode; fixture corrected
  and two runtime-observed cases added.

- [ ] **PPSA02929's submission rate collapses 15x after the splash.**
  Command buffers walked per 30 s: 655 then 43 (`PPSA02929_20260907_101945`).
  `FALSIFIED`: it stops drawing - it slows and continues. Established
  2026-09-07: the guest calls the builders for `WaitRegMem`, `DmaData` and
  `ReleaseMem` 247 times each, yet **none of those packets ever reaches the
  walker** (unsampled probe, zero occurrences), and the submit descriptor
  carries exactly one buffer, same as the reference - so nothing is being
  ignored. Those packets go into a buffer that is never submitted or is reset
  first; `sceAgcDcbResetQueue` runs once per frame and our walker treats it as
  a full shadow clear. `UNKNOWN`; next step is caller analysis of which buffer
  object those 247 calls target. Audit:
  `docs/audits/AUDIT-2026-09-07-submission-rate-collapse.md`.

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
  `sceAgcDmaDataPatchSetDstAddressOrOffset`), each called 247 times by
  PPSA02929. They write a real address into a previously emitted packet at an
  opcode-dependent field offset. Currently harmless only because the packets
  they patch never reach the walker; they become correctness-critical the
  moment that changes.

- [ ] **Intermittent emulated-TLS read failure at guest RIP `0x800160378`.**
  One run in four ends there with `Emulated TLS read failed ... not resuming
  the guest on a load that did not happen` (`PPSA02929_20260907_101044`).
  Newly reachable now that the title gets further. Intermittent, so capture
  several runs before drawing conclusions.

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

## Done (compressed; evidence in git log, docs/audits, docs/walkthroughs)

Shell, 2026-09-06 (v0.1.1):

- [x] Concept "Void console" implemented: theme tokens (ADR-003), dark/light/
  system, user accent and ground, corner style (rounded/sharp/cut) with
  content clipping; every screen on tokens.
- [x] Screens rebuilt to the artboards: Library hero + shelf + footer legend,
  All games (full-screen grid, search, five-way sort, favourites), Folder
  picker, Tools hub, Boot analyzer (select/search/analyze), Settings side
  column, Console, Booting screen (six stages, no spinner), Input tab
  (bindings-only 6-column grid), Controller testing popup.
- [x] 3D DualSense (Sketchfab model, CC-BY-4.0, headless Blender pipeline,
  20 parts, WPF Viewport3D): gravity tilt, trigger hinges, every button
  incl. share/options/mute with press + glow, sticks, touchpad, player LEDs,
  mute LED, lightbar colour; glyphs painted onto the cap discs.
- [x] Core: DualSenseWindows swaps accelerometer and gyro fields (0x0F/0x15);
  undone at the consumption point. Firmware report 0x20 VERIFIED on hardware.
  D-pad hat decode fixed. Battery >100% clamped (vendored-lib local mod).
- [x] Pad ABI in CoreBridge (state, firmware, audio levels, speaker/haptics
  tests, rumble, lightbar, LEDs); the shell's duplicate C# HID reader
  deleted (4.9); guest never sees the PCSX5-internal mute bit (test).
- [x] Controller-first navigation: spatial (TV-remote) movement on grids,
  Settings side-column, focus auto-scroll, Options = View all / restore
  defaults, crash-overlay mapping, credits popup from the README, pad ignored
  while the window is inactive, keyboard keeps working with no pad.
- [x] Tuning as settings rows (active gamepad, lightbar colour with picker,
  per-game configs, restore defaults), all persisted; lobby music volume
  setting; music stops on Play.
- [x] Launcher: IPC frame hook was per-module inline (null in the DLL) and
  headless never published a frame - fixed; launcher now embeds the core's
  real window (`--embed`), reparent host created on demand, swapchain
  recreated on resize, watchdog fed by IPC logs, deliberate stop is not a
  crash, boot phases from core log markers, game_state RUNNING on first frame.
- [x] Shell settings reached the core (`--config-dir` absolute); dev-build
  `pcsx5_cli.exe` lookup; "Add Directory" focus; compat status recorded in
  both places (tools/compat_report.py) and fetched after a run.

Core and harness, 2026-08/09:

- [x] Guarded-transfer results checked everywhere they were discarded
  (memcpy class, `WriteBuffer` retired, pad read/write paths, TLS store in
  the VEH, thunk writes); `.bss` protection fix; condvar signal race; shader
  cache key without ring address; `feof/fgets/fgetc`; DRAW_INDEX_OFFSET_2
  first-index; CB_COLOR0 base-ext at 0x390.
- [x] Harness tells the truth: own-window capture with timeout, refused
  samples counted, `boot_success` only for `progressing`, classifier locked
  by a 179-run replay test, baselines need 3 samples, `menu` marker retired,
  import report flushed every 30 s, guest-smoke tests can fail, clicks are
  window-relative, unknown keys refused.
- [x] `--play-input` was accepted and never read - wired; replay drives the
  harness. Six input backends exist (the "no keyboard path" claim was wrong).
- [x] Haptics over Bluetooth VERIFIED (report 0x32, init-prime required);
  audit in docs/audits.
- [x] Docs: missing directories, templates and `doc_links` CTest; Gemini
  rulebook citations removed after the owner deleted it.

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
