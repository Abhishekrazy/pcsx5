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
  `src/input/input_handler.h:199` defines `string_to_keyboard_key_map`, a
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

- [ ] **Execute every targetless draw, not just the last one** — THE FIX
  `st.pending_targetless` is a **single slot**, so all but the last targetless
  draw in a frame are overwritten. Measured loss: **87-88% of all guest draws**
  (2995/372 and 2715/352 across two runs). Now visible per-submit (`8b3bb45`).
  - [ ] Replace the slot with an ordered queue replayed at the flip
  - [ ] Test: a submit carrying N targetless draws executes N, not 1

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

- [ ] **Execute or explain the skipped AGC packets** - NOP `0x19` (DMA data),
  `IT_EVENT_WRITE` (`0x46`), op `0x76`.

---

## Phase 3 - Input, and the rest

- [ ] **Synthetic / keyboard input path in the core**
  No keyboard input exists anywhere in `src/` - no `GetAsyncKeyState`, no
  `WM_KEYDOWN`. No automated test can drive input at all, and any title waiting
  for a button needs physical hardware. Whole-project blast radius.
  - [ ] Synthetic pad state injectable from the harness
  - [ ] Keyboard bindings, analog sticks simulated from keys
  - [ ] Test: a scripted press reaches `scePadReadState`

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
