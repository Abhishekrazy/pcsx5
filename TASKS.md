# PCSX5 Tasks

Progress tracker. Updated as soon as a task completes, and whenever new work is
discovered. Each task states what it is, why it matters, and what *done*
requires — a task whose justification lives only in a chat log is not trackable.

Status: `[ ]` open · `[~]` in progress · `[x]` done · `[!]` blocked · `[-]` dropped or falsified

Priority is `(silent-failure risk × blast radius)`, then whether it makes later
work cheaper. **Broad fixes before single-title work**: a defect in shared
machinery outranks one game's next step.

---

## Done

- [x] **Register the zero-fill tail of a PT_LOAD with the segment's own protection**
  `Memory::Protect` stamped a sub-range's protection onto the whole tracked
  region, so a module's writable `.bss` was reported read-only and every guarded
  write into it silently did nothing. Commit `68387e9`, test
  `TestPartialProtectKeepsRestWritable` (2 failures before, passes after).

- [x] **Report a memcpy that did not copy** — `MemcpyImpl` discarded
  `Memory::GuardedCopy`'s result, turning a refused write into a successful
  no-op. This is what hid the defect above. Commit `4abc0bf`.

- [x] **Sweep the whole unchecked-guarded-transfer class** — nine further
  guest-facing sites (`memset`, `memmove`, `realloc`, `strcpy`, `strncpy`,
  `strcat`, libc `strcat`/`strncat`/terminator) all discarded their result.
  Commit `9466de8`.

- [x] **Stop losing condition-variable signals delivered before the waiter parks**
  117 of 200 lost in a targeted test; 0 after. Commit `77e6f9a`, test
  `TestCondvarSignalRace`.

- [x] **Keep the buffer base address out of the draw-program cache key**
  Five distinct shader pairs re-translated 2,536 times because the key included
  a per-frame ring-allocator address. Now 5. Commit `c2880f1`, test
  `TestDrawLayoutHashIgnoresBufferBase`.

- [x] **Implement `feof`/`fgets`/`fgetc`** instead of returning a fixed
  end-of-file under nine NIDs. Commit `0a36f3e`. *(IMPLEMENTED, not VERIFIED —
  no title exercises them; see open task on a guest-filesystem fixture.)*

- [x] **Capture the emulator's own window surface** rather than the screen
  behind it. The harness was photographing overlapping windows and produced a
  wrong verdict on a real 20-minute run. Commit `86052a7`.

- [x] **Bound the surface capture and account for refused samples**
  `PrintWindow` had no timeout (measured blocking 23s and 38s), and refused
  captures were uncounted — which biases the classifier *toward* "progressing"
  exactly when data is missing. Commit `65a0270`.

- [x] **Make the draw-execution loss visible** — per-submit accounting of draws
  executed vs dropped. Measured live at 207 executed / 432 dropped.

---

## Plan

A roadmap survey (7 subsystem surveys, 3 competing roadmaps, 2 judge panels)
settled on this spine:

> The unit of planning is a defect **class**, not a title and not a lifecycle
> stage - because every expensive defect this project has paid for was one class:
> a refusal wearing the costume of a success. A class is cross-title and
> cross-subsystem by construction.

Order: make the instruments incapable of lying, close the APIs where discarding a
result is the easy path, give threads and TLS a real owner, make fabricated HLE
success and dropped GPU work loud, then ratchet each swept class shut.

---

## Discovered — the WPF shell (2026-09-03, from a user screenshot)

- [x] **Shell settings had no effect on the emulator at all** — FIXED
  The core defaults `config_dir` to the *relative* path `"pcsx5_config"`,
  resolved against the child process's working directory. `IpcSession` pins that
  to the folder holding `pcsx5_cli.exe` — it has to, because the Windows loader
  must find `pcsx5_core.dll` before `main()` runs. The shell writes its config
  next to its own executable. So the shell saved to
  its own `pcsx5_config/global.json`, beside the shell executable, while the core read
  `build/bin/Release/pcsx5_config/global.json`.
  Measured: the shell's copy had `audio.backend: 1`, the core's had `0`. The
  user had turned sound on; the file saying so was never opened.
  Fixed by passing `--config-dir=<absolute>` to the child, a flag the core
  already supported. Verified end to end: with the shell's config dir the core
  logs `sceAudioOut: WASAPI mix is 44100 Hz` and opens a port; with the old
  default it never attempted audio.

- [ ] **WASAPI init fails on a 44.1 kHz output device and silently drops to
  waveOut.** Surfaced by the verification above:
  `WASAPI mix is 44100 Hz/2 ch (port wants 48000 Hz stereo)` →
  `WASAPI init failed; falling back to waveOut`. Sound works, but the shared-mode
  path a user selected is not what they get, and the reason is a resample the
  backend declines to do. Done means: resample to the device mix rate, or report
  the downgrade where a user can see it rather than only in the log.


- [ ] **The boot overlay is stuck at "Step 1 of 6 / 15%" while the console shows
  module linking.** Root cause found, two parts, both in `GameSession.cs`:
  1. There are **two parallel launch implementations**. `GameThreadProc` runs the
     core in-process through `pcsx5_init`/`pcsx5_load`/`pcsx5_run` and raises
     honest phases from the actual call it is inside. The shell does not use it:
     `Launch` (line 162) calls `_ipc.Launch(...)`, which spawns `pcsx5_cli.exe`
     as a **child process** over a shared-memory/pipe IPC. Two implementations of
     the same responsibility, and the dead one is the one with real progress.
  2. On the IPC path the phases come from "phase sniffing from log keywords"
     (`OnCoreLog`, ~line 358): `text.Contains("link") || text.Contains("reloc")
     || text.Contains("module")`. That is the bare-substring marker pattern
     `tools/game_runner/boot_markers.py` already documents as discredited and
     retired — it matched `menutitle-sheet0.png` for "menu". Worse, it is gated
     on `State == GameSessionState.Booting`, and `Launch` sets
     `State = Running` as soon as the child process starts, so **the sniffing
     never runs at all** and the overlay stays on its initial values.
  Done means: one launch path, and boot progress reported by the core over the
  IPC channel it already owns, rather than guessed from log substrings by the UI.

- [ ] **No menu music in the shell: audio defaults to Off and three config trees
  disagree.** `src/config/config.h:72` — `int backend = 0; // 0=Off, 1=WASAPI`.
  `pcsx5_config/global.json` (repo root) has `"backend": 0`;
  `dist/pcsx5_config/global.json` has `"backend": 1`; `.work/dbg_config/` is a
  third. `IpcSession` sets `WorkingDirectory` to the folder holding
  `pcsx5_cli.exe`, so which config the shell gets — and therefore whether there
  is any sound — depends on where the binary happens to live.
  `CoreBridge.Pcsx5Options` has no audio field at all, so the shell cannot
  override it even deliberately.
  Done means: the shell can set audio explicitly, and the config trees stop
  silently disagreeing about a user-visible default.

- [ ] **The shell passes `--headless` to the core it wants frames from.**
  `IpcSession.cs:122`. Flagged, not yet explained — it may be correct for the
  IPC frame-sharing path, but a shell that renders frames asking for headless
  needs a reason recorded next to it.

- [x] **The Gemini rulebook was deleted deliberately; dangling citations removed**
  `CLAUDE.md` and the `.claude/` rules, skills and agents cited 33 companion rule
  files and `GEMINI.md` as binding. The repository owner confirmed on 2026-09-05
  that they were deleted on purpose and are not wanted back, so the citations
  were removed rather than left pointing at missing documents — a contract that
  cites a document nobody has cannot be followed and cannot be checked.
  Anything stated only in that rulebook is no longer in force; the thirteen rules
  in `.claude/rules/` are now the whole of it.
  This was a real `doc_links` failure doing its job: it had been red locally
  since the deletion. `check_doc_links` now reports 202 referenced paths, all
  resolving. It passed on CI throughout, because `.claude/` is absent there too
  and neither set is scanned — worth remembering the next time CI green is
  mistaken for a clean tree.

## Phase 1 - Instruments that cannot lie (NOW)

Sweeping silent failure while measuring it with silently-failing instruments
produces unfalsifiable results.

- [x] **Two CTest cases passed while the process aborted** — FIXED
  `CMakeLists.txt` set `PASS_REGULAR_EXPRESSION` on `guest_syscall_smoke` and
  `guest_tls_smoke`; CMake documents that as *replacing* the return-code check,
  so neither could fail on a crash. Both now run through
  `tools/run_guest_smoke.cmake`, requiring the marker AND a zero exit AND no
  `FATAL:` line.
  - [x] Require a zero exit code as well as the marker
  - [x] Also reject a `FATAL:` line — needed because the emulator printed
        `FATAL: abort() raised (signal 22)` and then **exited 0**
  - [x] **Teardown abort root-caused and fixed.** `g_heartbeat_thread` is a
        static `std::thread`; `sys_exit` calls `ExitProcess`, whose
        `DLL_PROCESS_DETACH` runs its destructor while the thread is still
        parked on its 30s wait. A `std::thread` destroyed while joinable calls
        `std::terminate` — which *is* `abort()`, signal 22. The guest run had
        already succeeded, which is why the abort looked unrelated to it.
        The guest exit path now joins the thread first. Both tests pass
        legitimately; **suite is 52/52 and can now actually fail.**

- [x] **The `menus` progress marker is a false positive** — FIXED
  `session.py` matched the bare substring `"menu"`, so it fired on
  `images/menutitle-sheet0.png` — a filename being loaded, not a menu reached.
  Reported as evidence of progress throughout the recent single-title work.
  - [x] Marker table moved to `tools/game_runner/boot_markers.py`, imported by
        both `session.py` and `tools/autorun.py`, which previously kept separate
        copies "kept identical" by a comment — and both carried the bad entry
  - [x] Retired rather than narrowed: no substring of the current log
        distinguishes "a menu is on screen" from "a file with menu in its name
        was opened". `RETIRED_MARKERS` records why, so it is not reintroduced
  - [x] Baseline updated (documented golden change, Rule 07); a run afterwards
        reports `vs baseline unchanged`, so the loss of a false signal did not
        masquerade as a regression

- [x] **No run has ever produced an import report** — FIXED
  `sys_exit` called Win32 `ExitProcess`, which never returns and so never
  reached `pcsx5_shutdown`, the only caller of `PersistSummary`. 0 of 195
  archived runs contained `import_report.json` though 135 requested one. A guest
  exit hook now writes it before the process goes down.
  Before/after on the same ELF: report written **NO -> YES**.

- [~] **The stub-classification regime is inert**
  `RegisterStubContract` (`hle.cpp:462`) has no production callers, so
  `g_stub_contracts` is always empty and `GetStubContract` always returns
  `UNKNOWN`. Three call sites branch on that classification and none of them can
  ever see anything else.
  - [x] The inventory it depends on now exists. Reports were only written on a
        clean `sys_exit`, which no title run performs — they are killed by the
        harness timeout. A periodic flush (30s heartbeat) writes the import
        report regardless, and a killed PPSA02929 run now yields one:
        **165 stubs**, with a call-count heat map.
  - [ ] Populate contracts, which is Rule 04 contract-recovery work per symbol
        and does not belong in this phase. The inventory above is what makes it
        possible to do it in call-count order rather than arbitrarily.

- [x] **`boot_success` was true for a frozen run** — FIXED
  It was `status not in ("crashed","no-frame")`, so `frozen`, `exited` and
  `ran-headless` all counted as a successful boot. CLAUDE.md is explicit that a
  live process painting one unchanging frame is never reported as success, and
  that `ran-headless` means frame validation was impossible by construction.
  Now `status == "progressing"`.
  Two baseline entries claimed success while frozen (PPSA02929, PPSA21564);
  both corrected as a documented golden update.
  - [x] Stamp a `classifier_version` into every record and baseline entry, so a
        record judged under older rules is distinguishable rather than silently
        keeping its verdict

- [x] **Baseline promotion trusted a single sample** — FIXED
  `baseline --update` passed no stability study, and the three stability flags
  defaulted to `True`, so one run of a title whose status varies between crashed
  and frozen was written down as having a stable status — and every later
  comparison trusted it. They now default to **False**: one sample cannot
  establish stability, and only `measure` can.
  - [x] Refuse promotion from a record the current classifier did not judge.
        Verified: an unversioned record is refused with exit 1; a current one
        promotes normally.
  - [x] Baseline re-established for PPSA02929 from 3 samples — status frozen,
        `boot_success` false, stability flags earned rather than assumed.

- [x] **The run classifier is now locked against stored runs** — FIXED
  "progressing" is the only verdict this project treats as success, and it rested
  on four thresholds with no test. The risk is not a bug but an edit: loosening
  one to turn a red run green.
  - [x] Classifier extracted into a pure `classify_progression`, so it can be
        replayed over stored records rather than only over live runs
  - [x] `tools/check_run_classifier.py`, registered as the `run_classifier`
        CTest: replays **179 stored runs**, requires known-stuck runs to be
        rejected, and requires every threshold to be load-bearing
  - [x] Proven to catch an edit: quietly loosening `DISTINCT_RATIO_MIN` from
        0.5 to 0.05 turns it red; restoring turns it green
  - [x] No new dependency — plain Python and CTest, matching
        `check_nid_registrations.py` and `check_doc_links.py`, rather than
        adding pytest
  Note: `COVERAGE_MIN` gets a synthetic fixture rather than a corpus assertion.
  Every archived record predates `capture_failures`, so all replay at 100%
  coverage and the corpus cannot exercise that guard at all.

- [x] **Documentation paths cited by CLAUDE.md did not exist** — FIXED
  Seven paths every session is told to read led nowhere. `docs/tasks`,
  `docs/audits`, `docs/walkthroughs` and `docs/evidence` were absent entirely,
  while `RUNTIME_LIFECYCLE.md` and `PS5_BOOT_PIPELINE.md` live under
  `architecture/`. Rules 03/04/07/09 all discharge into "the owning audit",
  so those rules were unenforceable and nothing said so.
  - [x] Four directories created, each with a README stating its purpose
  - [x] References repointed across 12 files
  - [x] `docs/templates/{TASK,AUDIT,WALKTHROUGH}.md` created — the evidence
        skill cited templates that did not exist
  - [x] A cited audit that was never written is now a pointer to `docs/audits/`
        rather than a ghost filename
  - [x] `tools/check_doc_links.py`, registered as the `doc_links` CTest: 216
        referenced paths, all resolve. Proven to fail — a deliberately broken
        link makes it red, and removing it makes it green again.

---

## Resolved — the three items the first stub inventory raised

All three were investigated and none was a defect. Kept with their evidence so
they are not re-opened.

- [x] **`_Getptolower`, the hottest call in the emulator** — not a defect. It is
  implemented at `libkernel.cpp:2088` and builds its table exactly once under
  `std::call_once`; every later call returns a cached address. It is hot because
  the guest's Dinkum CRT calls it per case-insensitive operation, which a
  Construct engine parsing JSON does constantly. The only cost on our side is
  HLE dispatch, which belongs to the general performance task, not here.

- [x] **`9BcDykPmo1I`, "an unresolved NID called 308,960 times"** — my record was
  wrong. It is `__error`, the BSD per-thread errno accessor, it was already in
  `assets/nid_db.txt` at line 169, and it is implemented at
  `liblibc.cpp:2569`. Every `errno` check in the guest's libc calls it, so the
  count is expected.

- [x] **`5OqszGpy7Mg` is `strtoull`** — now VERIFIED rather than inferred from a
  matching call count. Recomputing the NID of "strtoull" yields `5OqszGpy7Mg`
  exactly; the same computation reproduces `_Getptolower` and `__error`, which
  checks the algorithm itself.

### The real defect underneath them

- [x] **The inventory printed raw NIDs for the symbols nobody can recognise**
  `ParseNidString` requires 11 encoded characters PLUS a 4-character tag, so a
  symbol registered as a bare NID never parsed and never resolved — which is
  exactly why `__error` read as an unknown and was recorded as one. The report
  path now decodes a bare NID too, and accepts it only when the table knows the
  value, so an 11-character *name* is left alone rather than renamed into
  something wrong. Three verified `strto*` NIDs added to the database.
  Top ten went from three opaque NIDs to none; 36 of 165 still show raw NIDs,
  which is honest — those names are genuinely unknown.

## Discovered — cached textures are never invalidated

- [ ] **A texture is cached by its descriptor, never by its contents**
  `TextureIdentity` (`src/gpu/vk_draw.cpp:403`) hashes guest address, width,
  height, pitch, format, tile mode, mip count and array shape — and nothing
  about the pixels. If the guest rewrites the data at that address with the same
  dimensions and format, the key is unchanged, the lookup hits, and the old
  image is displayed for the rest of the run. Nothing invalidates the entry: the
  only eviction is capacity (`> 512` entries clears everything).
  Blast radius: **many titles**. Anything that updates a texture in place —
  render-to-texture, animated or procedural textures, streamed atlases, dynamic
  UI, video-on-polygon — shows its first frame forever.
  **Both** reference emulators solve this the same way, arrived at
  independently, which is the strongest signal available that it is the right
  architecture: Kyty's `host_gpu/memoryTracker` + `pageManager` keep dirty
  ranges per page (`ValidateGpuDirtyPages`), and shadPS4's
  `video_core/page_manager.cpp` write-protects pages via
  `Protect(VAddr, size, MemoryPermission)` and invalidates on the fault.
  shadPS4 also carries `multi_level_page_table.h`, as does Kyty.
  - [ ] Confirm a title actually rewrites a texture in place before building
        page tracking — hash contents for small textures as a cheap probe first,
        and measure how often the hash changes for a fixed descriptor
  - [ ] Only then decide between content hashing and write-tracking; page
        protection is the expensive answer and should be justified by a
        measurement, not adopted because the reference has it

## Discovered — from the shadPS4 comparison

shadPS4 runs commercial PS4 titles, and PS5 shares most of the Orbis API
surface, so where it differs from us the difference is usually load-bearing.

- [ ] **Neither reference has a "targetless draw" concept — because both decode
      the colour-target registers**
  shadPS4 keeps `color_buffers[NUM_COLOR_BUFFERS]` in its register state
  (`video_core/amdgpu/regs.h:150`) with a full `ColorBuffer` layout
  (`regs_color.h:116`), and Kyty decodes the same registers. Our
  `pending_targetless` path exists only because `DecodeRenderTarget` never sees
  CB_COLOR0 — which is Phase 2's second task. This raises its priority: the
  fallback is not an architecture, it is a symptom, and no working emulator
  needs one.

- [ ] **Keyboard and mouse input mapped to the pad**
  shadPS4's `input/input_handler.h:199` defines `string_to_keyboard_key_map`, a
  configurable key-to-button binding, alongside `input_mouse.cpp`. We have no
  keyboard path at all. This is already Phase 3's first task; the reference
  gives it a concrete shape to follow rather than inventing one.

- [ ] **Audio decode is far thinner on our side** — LOW priority
  shadPS4 implements the Audio Job Manager with AT9 and AAC decoders
  (`core/libraries/ajm/`, ~2,137 lines). We have `libatrac9.cpp` at 370 lines
  registering 5 symbols. Recorded for completeness rather than urgency:
  PPSA02929 makes exactly **one** AJM/Atrac call in a full run, because it ships
  `.ogg`/`.flac` and decodes them itself. Worth doing when a title needs it,
  not before.

- [ ] **RECTLIST is mapped to a triangle strip** — latent, affects any title using it
  Kyty carries a dedicated `shader/rectListShader.cpp`. Our
  `PrimitiveTopologyFromVgt` maps VGT type `0x11` (DI_PT_RECTLIST) to
  `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP`. A rect list defines a rectangle per
  three vertices with the fourth corner computed, which a strip cannot express,
  so each rectangle renders as one triangle. Measured **not** to affect
  PPSA02929 — its draws are `vgt_primitive_type=0x4` (TRILIST), verified by
  logging at pipeline creation — so this is a real defect waiting for the title
  that uses it, not a current one.

## Phase 2 - GPU: stop discarding work

- [-] **FALSIFIED: executing every targetless draw makes the picture WORSE**
  Implemented and measured. The queue works mechanically — executed draws rose
  from 207 to 1317, and composites per flip went from always 1 to 9–12 — but the
  screen goes black:

      single slot (before)   37 of 37 captured frames had content
      queue       (after)     8 of 44, black from frame 8 onward
      after revert           39 of 39 had content again

  **Why it goes black is NOT established.** I first wrote that the earlier draws
  are intermediate passes being painted over the finished image. That was
  reasoning, not measurement, and a later probe contradicts it: the 9–12
  targetless draws in a frame are *identical in every respect we can observe* —
  same vertex and pixel shader, same source texture at the same address, the
  same 6-vertex full-screen quad, and the sampled texture's contents hash the
  same across all of them. They are not a chain of passes over changing data.
  A plausible alternative I have not tested: 6x the draw work on a title already
  at ~9 fps may simply stop frames being presented, and the capture then records
  black. Until that is measured, "the queue makes it worse" is the observation
  and the cause is UNKNOWN.
  Reverted (Rule 10) rather than kept and tuned.
  **What this means for the phase:** "stop discarding work" was the wrong frame.
  The draws are discarded because we cannot represent what they render *into*.
  The real question is whether these intermediate passes need offscreen targets
  of their own — which is a much larger design question than a queue, and one
  that should start from what the guest's shaders actually write.
  Draw accounting from `8b3bb45` stays: it is what made this measurable.

- [-] **FALSIFIED: "the colour-target binding is never observed" is not a defect**
  Measured directly by logging every context register the guest sets through the
  indirect patch path. It sets **54 distinct registers and not one of them is a
  colour-target register** — none of `0x318`, `0x319`, `0x31C`, `0x390`, `0x3B0`.
  The full set is shader state only: `SPI_PS_INPUT_CNTL` (0x191–0x1AF), shader
  configs (0x1B1–0x1C5), `CB_SHADER_MASK`, `CB_TARGET_MASK`, and no viewport,
  scissor, blend or raster mode either.
  So the guest genuinely never binds a render target; it composites straight to
  the scanout buffer. `DecodeRenderTarget` returning false is correct, and the
  targetless path is the right path for this title — which is precisely why
  SharpEmu, the emulator ours was modelled on, carries `PendingTargetlessDraw`.
  Separately noted while checking: `kCbColor0BaseHi = 0x319` looks wrong
  regardless. SharpEmu names the high bits `CbColor0BaseExt = 0x390`, and 0x319
  is absent from our own defaults table while 0x390 is present. Harmless today
  because no title in the fleet sets either, but wrong for one that does.

- [-] **ANSWERED: nothing differentiates them. They are duplicate draws.**
  Five independent measurements, each expecting to find the differentiator and
  each finding none:

  | measured | result |
  |---|---|
  | vertex + pixel shader addresses | identical across all draws |
  | source texture address and size | identical (0x215ed0000, 1280x720) |
  | source texture *contents* (hashed) | identical — one hash |
  | vertex buffer address, size, *contents* | identical — one hash |
  | `DRAW_INDEX_OFFSET_2` index offset | **0 on every draw** |

  So the 9–12 targetless draws per frame are byte-for-byte the same draw. The
  single slot loses nothing, executing one is equivalent to executing twelve,
  and the "87% discarded" figure counts redundant work rather than lost content.
  Phase 2's premise — *stop discarding work* — is retired: there is no work to
  recover.

  **And the draws are identical because the game is frozen.** It is redrawing an
  unchanging frame, so of course every draw matches. This whole line of
  investigation was measuring a consequence of the stall, not a cause. That is
  the useful result: the GPU path is not what is holding this title back.

  Two real defects were found along the way and are recorded separately —
  the texture cache never invalidating, and `kCbColor0BaseHi = 0x319`.

- [x] **DRAW_INDEX_OFFSET_2's first-index is now read** — IMPLEMENTED, not VERIFIED
  Our packet emitter writes `index_offset` at `packet+8`; the walker never read
  it back, so every such draw rendered from index 0 whatever slice the guest
  asked for. A title batching sprites into one index buffer and walking it by
  offset would draw its first primitive repeatedly.
  `ResolvedIndexAddr` now advances the index base by the first-index, and the
  snapshot starts there so the copied range is exactly the indices drawn.
  **No test.** One was written and abandoned: exercising it through
  `sceAgcDriverSubmitDcb` segfaults `hle_agc_tests`, because that path reaches
  real GPU work the test harness has no device for. A crashing test is worse
  than none, so it was removed rather than committed. Covering this needs a way
  to drive the walker without executing draws — worth building, and it would
  cover several other AGC behaviours at once.
  **No runtime evidence either:** every `DRAW_INDEX_OFFSET_2` in PPSA02929
  carries offset 0, measured across a full run, so the title cannot show the
  difference.

- [x] **CB_COLOR0 high address bits read from the right register** — IMPLEMENTED
  `kCbColor0BaseHi` was `0x319`, which is not a register — it looks like a guess
  at "the one after BASE". The high bits are `CB_COLOR0_BASE_EXT` at `0x390`:
  both reference emulators name it that, and **our own Gen5 defaults table
  contains 0x390 and no 0x319 at all**, which is the local evidence the old
  value was never real. Composition unchanged and matches GFX10:
  `(BASE_EXT << 40) | (BASE << 8)`.
  Unverifiable at runtime: no tracked title sets either register, so
  `vs baseline unchanged` is the most that can be said. ctest 52/52.

- [ ] **A way to drive the PM4 walker without a GPU**
  Testing anything in the walker currently means `sceAgcDriverSubmitDcb`, which
  reaches real GPU work and segfaults `hle_agc_tests`. Four routes around it were
  tried and all crashed — mapped index buffer, memory inside the command buffer,
  a zero index count, and recording state at parse time rather than at
  execution. `VkDrawExecute` is documented as a safe no-op without a device, so
  the crash is elsewhere in the submit path and has not been located.
  This blocks tests for the index-offset fix and for every other walker
  behaviour, so it is worth more than any single one of them.
  *Note:* the emulator arguably should not crash when walking a command buffer
  with no GPU present, so this may be a robustness defect rather than only a
  test-harness gap.

- [ ] **Execute or explain the skipped AGC packets** - NOP `0x19` (DMA data),
  `IT_EVENT_WRITE` (`0x46`), op `0x76`.

---

## Phase 3 - Input, and the rest

- [x] **CORRECTED: the input path already existed. `--play-input` was the gap.**
  I repeatedly claimed the core had "no keyboard input path at all — no
  `GetAsyncKeyState`, no `WM_KEYDOWN` anywhere in `src/`". That was wrong: I
  grepped for the wrong APIs. The window is GLFW, so keyboard input goes through
  `glfwGetKey`, and `src/gpu/input/glfw_keyboard_backend.cpp` maps 25 keys.
  There are **six** input backends — keyboard, DualSense, XInput, SDL
  GameController, a multiplexer, and `input_bot.cpp`, which synthesises
  controller state from a JSON replay. `--record-input` and `--play-input` are
  both documented in `--help`.
  **The real defect:** `play_input_path` was declared in `core_api.h` and set in
  `main.cpp` and **read nowhere**. The flag was accepted, advertised, and did
  nothing — the same silent-success class as the rest of this project's worst
  defects. Now wired: replay supersedes live devices, so a replay-driven run is
  reproducible and does not also pick up whatever is plugged in.
  Verified both ways: a valid replay logs `InputBot: loaded 3 events (120
  frames)` and `Input replay active`; a missing file logs the failure and says
  live input is unchanged rather than pretending.

- [x] **Drive the harness from replays** — DONE, and it needed no harness change
  `session.py` already had `--input`, which passes `--play-input=`, plus
  `--keys` and `--clicks` schedules. The pipeline was built end to end on the
  harness side and was dead only because the core ignored the flag.
  Verified: `session.py run --title PPSA02929 --input <replay>` logs
  `InputBot: loaded 3 events (120 frames)` and `Input replay active`.
  Worth noting what this means about the earlier defect — someone built the
  harness expecting replay to work, and it silently did not, for as long as the
  option went unread.
  **Still unverified at the guest level:** no tracked title calls
  `scePadReadState`, so nothing yet proves an injected button reaches guest
  code. Closing that needs either a title that polls the pad or a guest test ELF
  that does. **The guest-ELF route is not cheap**, which is worth writing down
  before someone reaches for it: `tests/test_elf/*.cpp` are freestanding clang
  binaries that talk to the emulator through raw `syscall` instructions. They
  have no PS5 import table, so they cannot reach `scePadReadState`, which is
  resolved by NID through `PT_SCE_DYNLIBDATA`. Using one as a pad instrument
  means hand-building a PS5-shaped import table first.

- [x] **The pad read path reported success for writes the guest never got**
  `scePadReadState` and `scePadRead` filled the guest's buffer through
  `Memory::WriteBuffer`, which is guarded but returns `void`. A buffer the
  emulator could not write to therefore produced a cheerful `0` from
  `scePadReadState` and `1 entry read` from `scePadRead`, while the guest kept
  whatever bytes were already in its buffer — a controller frozen mid-state,
  with no error to test and nothing logged where it broke.
  The memory subsystem was already detecting it and saying so
  (`GuardedWrite: invalid write at 0x... (copied 0 of 120 bytes)`); the pad code
  simply ignored the answer. That is the whole defect, and it is the same shape
  as `MemcpyImpl` and as `--play-input` being accepted and unread.
  Both now check the guarded write. `scePadRead` additionally counts only
  entries the guest actually received, so failed writes no longer consume ring
  samples the guest never saw.
  Test: `TestPadRejectsUnwritableBuffer` in `tests/hle_audio_pad_tests.cpp`
  maps a `PROT_READ` page and passes it in. Before: 2 failures (`lhs=0` for
  ReadState, `lhs=1` for Read). After: suite passes, with
  `scePadRead: entry 0 unwritable at 0x... (0 of 120 bytes); returning 0`.
  INFERRED, and flagged as such in the code: returning the invalid-argument
  error for an *unwritable* rather than null buffer matches the null case by
  analogy. What retail firmware does here is UNKNOWN — on hardware the store
  would most likely fault inside the guest rather than return at all.

- [x] **`scePadGetData` wrote through a raw, unguarded host store** — FIXED
  `src/hle/libscepad.cpp` zeroed 64 bytes of the guest's buffer with a loop of
  `Memory::Write<u64>`, which is `*reinterpret_cast<T*>(addr) = value`: no page
  check, no demand-commit, no failure report. Rule 05 forbids exactly this.
  Evidence that it mattered: a `PROT_READ` guest page is genuinely host
  read-only — a plain `memset` to one terminates the process (exit 139) — so
  this store was writing into memory whose protection nothing had consulted.
  Now a checked `GuardedWrite`. Before: `GetData(unwritable buffer)` returned 0,
  reporting success (`lhs=0`). After: returns the invalid-argument error and
  logs `scePadGetData: buffer 0x... unwritable (0 of 64 bytes)`.
  **Two things deliberately left alone**, both noted in the source so they are
  not mistaken for settled: the 64-byte length is inherited and probably wrong
  (`ScePadData` is 0x78), but fixing it needs a caller observed at runtime, not
  a guess made while fixing something adjacent; and the error code is INFERRED
  by analogy with the null-pointer case — what retail firmware returns for an
  unwritable buffer is UNKNOWN.

- [x] **`Memory::WriteBuffer` retired in favour of `GuardedWrite`** — DONE
  The user chose removal over `[[nodiscard]]`, and removal proved the better
  call for a reason worth recording: the compiler then enumerated every call
  site, so none could be missed. It found 26 in `src/` **and two in `tests/`**
  that a `src`-only search had not — including `memory_validation.cpp`, which
  was round-tripping through the very wrapper being removed.
  Conversions were not uniform. Two were more than mechanical:
  - `kernel.cpp` emulates guest TLS stores inside the VEH: it wrote, advanced
    RIP, and resumed. A failed store meant the guest resumed as though it had
    happened, and its next read of that slot returned a stale value with nothing
    recorded. It now refuses to resume on a store that did not land.
  - `hle.cpp` wrote a thunk's machine code and returned the address regardless,
    handing the guest a pointer to whatever happened to be there. Now returns 0.
  The rest are out-parameter writes returning their module's existing
  invalid-argument error instead of a plausible success. No error codes were
  invented.
  The tests were strengthened, not weakened: `memory_validation` now asserts
  `GuardedWrite`'s return value and byte count, including across a page
  boundary — an assertion the `void` wrapper made impossible.
  `memory.h` carries a note saying what was removed and why, so the wrapper is
  not reintroduced as a convenience.
  Verified: 51/52 ctest (only the unrelated `doc_links`), 0 warnings,
  PPSA02929 unchanged against baseline.

- [ ] **`Memory::ReadBuffer` has the identical flaw, at 38 sites**
  It wraps `GuardedRead` and discards the result: `kernel.cpp` (13),
  `vk_draw.cpp` (6), `elf.cpp` (5), `libkernel.cpp` (3), `libatrac9.cpp` (3),
  `guest_printf.cpp` (2), and one each in `libkeystone`, `libaudioout`,
  `libagc`, `xa2_device`, `wasapi_device`, `audio_device`.
  Arguably worse than the write case: a failed write leaves the guest with stale
  data, while a failed read hands *our own code* plausible-looking garbage and
  it proceeds on it. `elf.cpp` reading headers and `vk_draw.cpp` reading vertex
  data are the ones to look at first.
  Not started: the user approved retiring `WriteBuffer` specifically, and
  extending that to `ReadBuffer` is their call, not an assumption to make.

- [x] **FALSIFIED: "DualSense vibration, lightbar, mic-mute and analog sticks
  are broken."** Tested against real hardware on 2026-09-05, connected over
  **Bluetooth** (VID 054C / PID 0CE6 via `BTHENUM`), using a new manual probe,
  `tools/dualsense_probe.cpp`. Every one of them works in the core:
  ```
  left  stick x:[3..255] y:[0..255]     right stick x:[0..255] y:[0..255]
  triggers  L2 max=255  R2 max=255      buttons 0x0010FFF0   touch 1 finger
  ```
  and the user confirmed all four outputs physically occurred: lightbar colour
  cycle, both rumble motors, mic-mute LED, player LEDs.
  The earlier report was accurate when made; replacing the guess-the-offsets
  in-header reader with the vendored DualSenseWindows library fixed it. The
  entry is kept, marked falsified, so nobody re-investigates a solved problem.
  The probe is deliberately **not** a CTest: it needs hardware and a person to
  confirm what they saw, so as an automated test it could only pass vacuously.

- [ ] **The shell has a second, competing DualSense reader — this is where the
  breakage the user saw actually lives.** `src/ui_csharp/WindowsDualSenseReader.cs`
  (767 lines) is a complete independent C# HID implementation, and the shell's
  Controller Setup uses it — `CoreBridge.cs` exposes no pad state at all.
  Both it and the native reader open the device with
  `FILE_SHARE_READ | FILE_SHARE_WRITE`, so neither blocks the other; they
  **interleave**. Two consumers of one HID input stream each receive a share of
  the reports, and two writers overwrite each other's output state. That fits
  "works standalone, broken in the app" precisely.
  HYPOTHESIS, not yet confirmed: the next step is to open Controller Setup with
  nothing else running. If it works alone, interference is the cause; if it is
  still broken, the C# reader has a defect of its own.
  Either way the duplicate should go (ADR-001), but this decides urgency.
  **Blocked on approval:** exposing pad state through `CoreBridge` is a public
  native ABI change, which CLAUDE.md makes a stopping condition.

- [x] **Battery percentage above 100% — fixed in the vendored library**
  `DS5_Input.cpp` computed `(nibble * 100) / 8` on a 4-bit field, so it could
  report up to 187%. Real readings of 125% and 112% are what exposed it.
  Clamped to 100 rather than rescaled: the true scale of that nibble is
  `UNKNOWN` here, and guessing a divisor would swap a visibly wrong number for
  an invisibly wrong one. Recorded as a local modification with a removal
  condition in `third_party/DualSenseWindows/README.md`, as the vendoring
  policy requires.

- [x] **DualSense haptics over Bluetooth - WORKING, verified on hardware**
  A 3-second tone at 60 Hz, 150 Hz and 30 Hz each produced felt vibration on a
  Bluetooth-connected DualSense. Reproduce: `build/Release/dualsense_probe.exe`.
  Implemented as `PlayHapticsPcmBlocking` in `src/gpu/dualsense_ds5w.cpp`:
  report `0x32`, 142 bytes, s8 stereo PCM at 3000 Hz, 64 bytes per report every
  ~10.67 ms, preceded by an init-prime and 8 silence reports.
  This closes a HARD BOUNDARY I declared and then falsified twice. Full account
  in `docs/audits/AUDIT-2026-09-05-dualsense-audio-over-bluetooth.md`.
  The failure worth carrying forward: the second attempt sent 282 reports, all
  accepted, and did nothing, because the stream was never opened by an
  init-prime. The device returned success for work it discarded - the same shape
  as every other defect this project has hunted, arriving from the hardware
  rather than from our own code.

- [ ] **DualSense speaker audio over Bluetooth** - a different lane from
  haptics: report `0x35`, 334 bytes, carrying a 200-byte **Opus** frame
  (48 kHz stereo, 160 kbps CBR). A combined `0x36` report carries state, haptics
  and audio together.
  Blocked on a decision rather than on knowledge: this needs an **Opus encoder**,
  a new third-party dependency, which needs an owner, version pin, licence check
  and an ADR. Do not add one without that.

- [ ] **DualSense microphone input over Bluetooth - still `UNKNOWN`**
  Every source examined covers the output direction only. It is a different data
  path and must not be assumed to work because output does.

- [ ] **Wire haptics into the emulator, not just the probe.**
  `PlayHapticsPcmBlocking` blocks for the duration of the audio, which suits a
  test and not a running game. Production use needs a streaming interface fed by
  the guest's haptics output on its own paced thread.

- [ ] **ADR-001 steps 2–4 remain**

- [ ] **A guest-filesystem test fixture** - blocks tests for `feof`/`fgets`/
  `fgetc` and for the guarded-transfer reporters.

- [ ] **Save-data mount returns an empty mount point** - the guest builds
  `/-saveindex` with no prefix; `sceSaveDataSetParam`/`SaveIcon` are stubs.

- [ ] **Performance: ~9 fps against a reference emulator's 60 fps.** Measure
  honestly first: flip rate is not uniform, so flips divided by duration compares
  nothing across runs of different lengths.

---

## Later

- [ ] DualSense: vibration, lightbar, mic-mute, analog sticks not registering,
  misaligned diagram overlays, no live test panel. Needs the user's hardware.
- [ ] Multiple controllers - never exercised.
- [ ] UI ratchet - 229 hardcoded XAML strings, 0 of 119 controls with
  `AutomationProperties.Name`, 0 `DynamicResource`.
- [ ] Committed build outputs under `src/ui_csharp/bin/**`.
- [ ] `tools/dream_tool.py` targets the wrong title's eboot.
- [ ] `GetWindowRect` includes the DWM resize border (~7px);
  `DWMWA_EXTENDED_FRAME_BOUNDS` is correct for the fallback path.

---

## Falsified — kept so they are not re-attempted

- [-] **"The 0xFFFFFC18 condvar timeout is a negative duration we misread"** —
  the reference implementation types it `unsigned int`, waits the same ~71
  minutes, and runs the title anyway.
- [-] **"The half-black quad is a GPU rendering defect"** — the splash renders
  perfectly at frame 12; the diagonal is a wipe transition frozen part-way, a
  symptom rather than a cause. Topology, culling and vertex count were each
  measured correct.
- [-] **"The guest never calls `sceKernelWaitEqueue`"** — it calls it once per
  frame. The wait accounting only counts the *blocking* path.
- [-] **"End-of-pipe release-mem is never executed, so the guest waits forever"**
  — the packet is genuinely unhandled, but its data-selection field is zero on
  every occurrence, so no write is requested and skipping it is correct.
- [-] **"The title is merely slow, not stuck"** — twenty minutes produced no new
  guest output at all.
- [-] **"Background loading stalled at 49 of 65 media files"** — the 16 unopened
  files are music, streamed on demand rather than preloaded.
