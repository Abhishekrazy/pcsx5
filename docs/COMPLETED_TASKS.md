# PCSX5 - completed and partially completed tasks

Moved out of `TASKS.md` on 2026-09-08 so that file lists only work that is still
open. Nothing here was deleted: an entry's justification, measurement and
evidence are what make it checkable later, so they are preserved verbatim under
the section they came from.

`[x]` is complete against the definition of done in `CLAUDE.md` - built,
tested, evidence cited. `[~]` would mark partial work; there is none right
now, the last one having been finished on 2026-09-08.

Falsified hypotheses are **not** here; they remain in `TASKS.md`, because they
are guidance for open work rather than finished work.

### From: Open - shell and launcher
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
- [x] **Release packaging: Squirrel assets are now built by the script.**
  Completed 2026-09-08. `build_release.ps1` gained `-Squirrel`, plus
  `-ReleaseDir` (default `releases\`) and `-SquirrelExe`. It locates the newest
  `Squirrel.exe` in the NuGet package cache, stages the release, and runs
  `pack --allowUnaware` to produce RELEASES, the full nupkg, a delta against the
  previous full package, and Setup.exe - the assets the in-app
  `Squirrel.UpdateManager` (`MainWindow.xaml.cs:395`) reads and which previously
  nothing in the repo could reproduce.

  The zip and the Squirrel package now share one `New-ReleaseStage` function, so
  the two cannot drift: both ship the same allowlist rather than `dist\*`
  wholesale, which is what put 345 MB of caches, crash dumps and the user's own
  config into the v0.1.1 zip.

  Failure paths are explicit rather than silent: `-Squirrel` without `-Version`
  stops with the reason (the version drives the update comparison and cannot be
  guessed from the build), a missing or bad `Squirrel.exe` stops with the
  install command, a missing prior full nupkg warns that only a full package
  will be produced, and the outputs are verified after the pack instead of
  assuming success.

  Verified end to end into a scratch release directory seeded with the real
  v0.1.0 package: full `pcsx5-0.1.3-full.nupkg` (94.1 MB), **delta
  `pcsx5-0.1.3-delta.nupkg` (80.9 MB) built 0.1.0 to 0.1.3**, `pcsx5Setup.exe`
  (94.3 MB), and a RELEASES listing all three with hashes. Both failure paths
  were exercised and exit non-zero. The real `releases\` directory was not
  touched. Script parses clean under the PowerShell AST parser.

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
### From: Open - core
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
- [x] **RESOLVED: the post-splash black screen.** Done 2026-09-07 (commit
  99fedde). A draw sampling a guest address was always satisfied by uploading
  that address from guest memory, but when the guest renders into a surface
  the pixels exist only in the Vulkan image - guest memory there is never
  written. PPSA02929's post-splash scene is a render-to-texture chain whose
  final full-screen pass samples such a surface, so it painted black over a
  frame we had otherwise drawn correctly. A sampled binding naming a live
  render target now binds that target's image; render targets sit in
  `VK_IMAGE_LAYOUT_GENERAL`, valid both as attachment and sampled source,
  because Vulkan forbids a transition inside an active render pass.
  60 s: status `frozen` -> **`progressing`**, unique frames 5 -> **29 of 29**,
  longest freeze 38.3 s -> 6.1 s; 53/53 ctest. Baseline updated from
  `PPSA02929_20260907_123027`. Audit:
  `docs/audits/AUDIT-2026-09-07-render-target-sampling.md`.
- [x] **RESOLVED: linear textures were read with the wrong row stride.**
  Done 2026-09-07. Every image descriptor this title submits has an empty
  pitch field, and the decoder substituted the surface width - which asserts
  that rows are tight. Hardware pads each linear row to 256 bytes, so widths
  already aligned (1280, 320) rendered correctly while unaligned ones (980,
  250) sheared by one rows worth per row. The title logo dumped as noise
  before and reads as "Dreaming Sarah" after. An absent pitch now decodes as
  unspecified and the upload derives the padded stride where the element size
  is known. 6 of 6 runs progressing with every frame unique; 54/54 ctest. One
  existing assertion pinned the defect and was corrected. Audit:
  `docs/audits/AUDIT-2026-09-07-linear-texture-row-stride.md`.
- [x] **Vulkan validation runs, and three render-path violations are fixed.**
  Done 2026-09-07. The Khronos layer was force-enabled through the loader with
  synchronization validation. Fixed: a read-after-write hazard at
  `vkCmdBeginRenderPass` (self-inflicted - moving render targets to GENERAL so
  a pass can sample what it drew left the seeding barrier naming only shader
  stages); storage buffers without NonWritable in both stages (the shaders
  really can write, so `fragmentStoresAndAtomics` and
  `vertexPipelineStoresAndAtomics` are now requested when supported); and
  SPIR-V 1.5 emitted against a Vulkan 1.1 target environment (the instance now
  requests 1.2, falling back to 1.1). 26 validation errors cleared to 0 on the
  render path. 54/54 ctest, 6/6 runs progressing with 22/22 unique frames.
  Audit: `docs/audits/AUDIT-2026-09-07-vulkan-validation.md`.
- [x] **RESOLVED: presentation-loop synchronization.** Done 2026-09-07. Four
  distinct defects, all verified against the validation layers: the acquire
  ran before the fence wait so the acquire semaphore was reused with a wait
  still pending; one shared done semaphore was re-signalled while a present
  could still be waiting on it (now one per swapchain image, since
  re-acquiring image i proves its wait completed); the letterbox clear and the
  blit both wrote the swapchain image with no dependency between them; and the
  acquire layout barrier used TOP_OF_PIPE while the semaphore is waited at
  TRANSFER, in all three present paths. Separately the draw fence was reset
  only when a batch was in flight, so the first submit passed a signalled
  fence. Validation findings 33 to **0**, no new ones; 54/54 ctest.
  Audit: `docs/audits/AUDIT-2026-09-07-presentation-synchronization.md`.
- [x] **LOCALIZED: both remaining title-screen artifacts come from one pass.**
  Done 2026-09-07. Captured every stage of the chain by presenting each render
  target in turn. Source texture clean; intermediate 1 clean at two frames
  (crisp, correctly proportioned logo); **intermediate 2 shows both the warp
  and the top-region blocks**; intermediates 3 and the display carry them
  forward unchanged. The pass graph shows `ps 0x215795900` is used exactly
  once in the whole chain and is the draw that reads intermediate 1 and writes
  intermediate 2. Every other title-screen pass is a plain textured quad. All
  stages are 1280x720 R8G8B8A8_UNORM single-sample, so scaling, format
  conversion and multisample resolve are excluded. Root cause deliberately not
  pursued; no production code changed. Audit:
  `docs/audits/AUDIT-2026-09-07-artifact-localization.md`.
- [x] **AUDITED: guest shader `0x215795900` is faithfully translated.**
  Done 2026-09-07, read-only, no production code changed. The guest program is
  238 instructions, 1208 bytes, zero unknown opcodes; the generated SPIR-V is
  74 KB and passes `spirv-val --target-env vulkan1.2`. Every arithmetic class
  balances exactly: 33 cosines, 7 sines, 43 fused multiply-adds, 35 adds, 16
  subtracts, 4 reciprocals, 2 half-packs and 1 texture sample all map
  one-to-one. The only count that differed, 122 multiplies against 82 in the
  guest, is explained precisely by the 40 turns-to-radians conversions the
  trig lowering requires - and that conversion is present and correct, which
  is the obvious way this would have gone wrong. **Case A: semantic
  equivalence established.** Do not modify the translator on this evidence.
  Audit: `docs/audits/AUDIT-2026-09-07-shader-215795900-audit.md`.
- [x] **DONE: dump the uniform constants this pass loads.** Instrumented
  `libagc.cpp` to print every 128-byte constant block bound to the pass whose
  program contains 20 or more `VCosF32`, every 25th draw, over a 120 s run of
  PPSA02929 (`PPSA02929_20260908_003546`). Two blocks are bound, not one.
  Block A is constant across all 475 sampled draws:
  `1.358 0 0 0 0 2.4142 0 0 0 0 -1.002 -1 0 0 -2.002 0 0.11506 0`.
  Block B varies in exactly one slot: index 12 runs `0.016666` at the first
  sample to `1.7333` at the last. All values finite; instrumentation removed.
- [x] **DONE: remove the per-draw storage copy by importing guest memory into
  the device.** `VK_EXT_external_memory_host` backs a `VkBuffer` directly with
  the guest pages, so the descriptor reads guest memory in place and there is
  no cached copy that can go stale. `ImportGuestRange` in `vk_draw.cpp` aligns
  the range down to a page, imports the window, and caches it by base; ranges
  the device cannot import fall through to the existing copy, so nothing
  regresses without the extension. Coverage: **84% of binds, 72% of bytes**;
  every rejection is the one case where the offset within the page is not a
  multiple of `minStorageBufferOffsetAlignment` (16 here). Storage-buffer phase
  **21.0 s to 0.20 s** over 1200 draws; steady-state **1.0 fps to 2.9-4.7**
  across five runs; peak 78 to 118. 54 of 54 tests, 0 validation errors,
  `progressing` with 29 of 29 unique frames. Audit part 3.
- [x] **DONE: HLE dispatch bookkeeping cost 157 microseconds per call.** The
  title makes **2,262,337 HLE calls per 60 s run**, dominated by C runtime
  parsing - `__error` 307,954, `_Getptolower` 239,683, `strtoull` 151,181,
  `memcpy` 138,825 - which is the load hitch before the menu. Dispatch is a
  direct jump, not an exception, so the cost was in the dispatcher: it copied
  the whole `HleSymbol` by value (two strings plus a `std::function`), built
  two more strings via `SafeString` for a normally-disabled `LOG_DEBUG`, copied
  two more into the trace ring, and then took a **global exclusive mutex** in
  `RecordStats` to re-assign fields that never change after the first call.
  About six heap allocations per dispatch, serialised across guest threads -
  measured at 130 microseconds rising to 157 as thread count grew. Fixed in
  `src/hle/hle.cpp`: one shared lock, handler-only copy, `SafeString` moved
  into the branches that format, `TraceEntry` carrying `symbol_id` with names
  resolved in `GetImportTrace`, and `RecordStats` filling descriptive fields
  once. Result **2.9-4.7 fps to a tight 4.3-4.6**, 54 of 54 tests, import
  report names still resolved. Audit part 4.
- [x] **DONE: `Memory::IsReadable` cost 95 microseconds per call - the single
  largest remaining cost.** It calls `Query` once per 4 KB page, and `Query`
  issues a **`VirtualQuery` syscall** then takes the global `g_regions_mutex` to
  scan the region table linearly. Measured in the shader evaluator `TryReadU32`:
  60,000 calls, **5,714 ms in `IsReadable` against 1.5 ms in the guest read it
  guards** - roughly 3600x the thing it protects. An 8-dword descriptor load paid
  it eight times. This also explains why validating a 1 MB import window per bind
  cost more than the copy it replaced (256 syscalls). Fixed with a thread-local
  512-entry direct-mapped page-state cache in front of `Query`, invalidated by a
  global `g_map_generation` counter bumped at all 16 public mutating entry points
  (bumps live there, not at the 43 Win32 calls, because that is the smaller set
  to keep complete). Result: **4.4-4.7 fps to 9.3-9.8**, unique frames **20-22 to
  25-26**, and every run `progressing` where about half were `frozen`. 54 of 54
  tests, 0 validation errors. Audit part 6.
- [x] **DONE: `IsWritable` and `IsExecutable` given the same cached path.**
  Structurally identical to `IsReadable`. Required closing an invalidation hole
  first: `ArmWriteRangeLocked` / `DisarmWriteRangeLocked` change page protection
  and are reached from the **VEH fault path**, which passes through no public
  entry point, so the generation bump had to go inside those two functions - 18
  bump sites total. Benign for reads, wrong for writes, hence done before
  extending the cache.
- [x] **DONE: guarded writes into write-tracked ranges are no longer
  discarded.** The latent defect recorded earlier became live once the emulator
  was fast enough to reach gameplay: **61 guest writes of 128 bytes silently
  dropped in one run** (`GuardedCopy: write fault ... copied 0 of 128 bytes`).
  Fixed with `Memory::ReleaseTrackedWriteAt`, called at all six guarded write
  sites - the software equivalent of the hardware fault path. Regression test
  `TestGuardedWriteIntoTrackedRange` asserts the bytes land *and* the write
  generation advances; verified failing before (4 failures, 0 of 128 bytes
  written) and passing after. Post-fix runs show **0** dropped writes.
- [x] **DONE: fullscreen from CLI and config.** `graphics.fullscreen` was parsed
  by the config loader and **never read**, so the option existed in the file and
  in the UI while doing nothing; there was no CLI flag either, only the F11
  runtime toggle. Added `--fullscreen` plus honouring the config value at window
  creation, both routed through the same `ToggleFullscreen()` F11 uses so the
  two cannot disagree.
- [x] **DONE: 1% low frame rate, and MB/GB units in the readout.**
  `Diagnostics::GetOnePercentLowFps` averages the slowest 1% of frame intervals
  (taken from consecutive frame timestamps, since the per-frame total only
  covers instrumented stages). The window readout now shows
  `100.3 fps (1% low 77.8) | 9.97 ms | 1486 draws/s | CPU 4% | VRAM 106 MB/14.87 GB | RAM 444 MB`,
  switching to GB above a gigabyte.
### From: Requested features, not yet started
- [x] **DONE: establish a trustworthy frame-rate metric.** Flip counts in the
  run log are unusable (482/394/483 across three identical runs). The `FPS:`
  line that `core_api.cpp` already emits once a second via
  `Diagnostics::LogFrameTimingStats` is reliable and agrees with the guest own
  frame accumulator to within rounding. Use it for all future progression
  measurement.
- [x] **RESOLVED: the intermittent null guest thread pointer.** Done
  2026-09-07 (commit pending). A patched TLS site loads the guest thread
  pointer from a host TLS slot and used the value unchecked. That slot is zero
  on any host thread that reached guest code without being bound, so
  `mov rax, fs:[0]` produced 0 and the guest dereferenced a null-based
  address - PPSA02929 died at guest RIP `0x80015ff6d` executing
  `mov r12, [rax - 0x15b8]` with `RAX = 0`
  (`PPSA02929_20260907_133921`). The exception-handler path this stub replaced
  resolves the same access through a three-level chain ending at the shared
  block, so the two disagreed on exactly this case although `cpu.cpp` states
  they must agree. The stub now tests for zero and falls back to the same
  pointer, preserving EFLAGS with `pushfq`/`popfq`, and refuses to emit at all
  if the fallback would be zero. 12 of 12 clean 45 s runs after, against 11 of
  12 before; 54/54 ctest. Audit:
  `docs/audits/AUDIT-2026-09-07-tls-stub-null-thread-pointer.md`.
- [x] **RESOLVED: the `Emulated TLS read failed` variant was a decode race.**
  Done 2026-09-07. The diagnostic added for this caught it on the next run:
  the thread pointer was fine and page-aligned, but the decoded displacement
  was `0x90909090` - four NOP bytes - where the instruction
  (`mov rax, fs:[0]` at `0x800160378`) has zero
  (`PPSA02929_20260907_145600`). The handler decoded the faulting instruction
  byte-at-a-time from live memory while another thread was rewriting that same
  site as a call plus NOP padding, so it read an original opcode with an
  already-overwritten displacement field. `TlsPatch::ReadInstruction` now
  snapshots the instruction under the patch lock and the handler decodes that.
  6 of 6 clean 45 s runs; 54/54 ctest. Audit:
  `docs/audits/AUDIT-2026-09-07-tls-stub-null-thread-pointer.md` section 10.
### From: Done (compressed; evidence in git log, docs/audits, docs/walkthroughs)
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
