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

- [~] **Two CTest cases pass while the process aborts** (VERIFIED here)
  `CMakeLists.txt` set `PASS_REGULAR_EXPRESSION` on `guest_syscall_smoke` and
  `guest_tls_smoke`; CMake documents that as *replacing* the return-code check.
  Both now run through `tools/run_guest_smoke.cmake`, which requires the marker
  AND a zero exit AND no `FATAL:` line.
  - [x] Require a zero exit code as well as the marker
  - [x] Also reject a `FATAL:` line — needed because the emulator prints
        `FATAL: abort() raised (signal 22)` and then **exits 0**, so an exit-code
        check alone still could not catch it. (The roadmap survey reported exit
        2; measured here it is 0.)
  - [~] **Root-cause the teardown abort now exposed.** ROOT CAUSE FOUND:
        `SysExit` (`syscalls.cpp:229`) calls Win32 `ExitProcess(status)`. That
        commits the exit code *before* teardown, which is why the process reports
        0 while announcing a fatal abort, and it terminates worker threads at
        arbitrary points before DLL detach, which is where the abort comes from.
        The remaining work is to shut down in the documented order instead of
        abruptly — a lifecycle change (Rule 06), so it is deliberately not
        bundled with the reporting fix. Both tests are red, which
        is correct and must not be suppressed (Rule 07). The guest itself
        succeeds — it prints its marker and calls `sys_exit(status=0)` — and the
        abort fires *after* that, during emulator teardown. Suite is now 48/50
        with 2 legitimately red.

- [ ] **The `menus` progress marker is a false positive** (VERIFIED here)
  `session.py:76` matches the bare substring `"menu"`, so it fires on
  `images/menutitle-sheet0.png` - a filename being loaded, not a menu reached.
  Cited as evidence of progress throughout the recent single-title work.
  - [ ] One shared marker vocabulary for `session.py` and `tools/autorun.py`
  - [ ] Record the loss of a false signal as such, not as a regression

- [x] **No run has ever produced an import report** — FIXED
  `sys_exit` called Win32 `ExitProcess`, which never returns and so never
  reached `pcsx5_shutdown`, the only caller of `PersistSummary`. 0 of 195
  archived runs contained `import_report.json` though 135 requested one. A guest
  exit hook now writes it before the process goes down.
  Before/after on the same ELF: report written **NO -> YES**.

- [ ] **The stub-classification regime is inert** - `RegisterStubContract`
  (`hle.cpp:462`) has no production callers, so `GetStubContract` always returns
  UNKNOWN.

- [ ] **`boot_success` is true for a frozen run** - `session.py:654` defines it
  as `status not in ("crashed","no-frame")`, contradicting CLAUDE.md.
  - [ ] Stamp a `classifier_version` into every record and baseline entry

- [ ] **Baseline promotion trusts a single sample** - `baseline --update` resets
  every stability flag to true from one run; a stored PPSA21564 run (29 frames,
  3 unique, 42.3s freeze in 60s) is recorded as *progressing*.

- [ ] **Python test infrastructure (the repository's first)** - lock the run
  classifier against the 175 stored `record.json` fixtures, so loosening any
  threshold fails a test.

- [ ] **Documentation paths cited by CLAUDE.md do not exist** - `docs/tasks`,
  `docs/audits`, `docs/walkthroughs`, `docs/evidence` are absent;
  `RUNTIME_LIFECYCLE.md` lives under `architecture/`. Rules 03/04/07/09 all
  discharge into "the owning audit", which has nowhere to go.

---

## Phase 2 - GPU: stop discarding work

- [ ] **Execute every targetless draw, not just the last one**
  `st.pending_targetless` is a **single slot**, so all but the last targetless
  draw in a frame are overwritten. Measured loss: **87-88% of all guest draws**
  (2995/372 and 2715/352 across two runs). Now visible per-submit (`8b3bb45`).
  - [ ] Replace the slot with an ordered queue replayed at the flip
  - [ ] Test: a submit carrying N targetless draws executes N, not 1

- [ ] **Recover why the colour-target binding is never observed**
  CB_COLOR0 registers `0x318`/`0x319`/`0x31C`/`0x3B0` appear in zero of 784
  captured submits; 2477 indirect context-register writes per frame collapse to
  53 distinct registers, with no viewport, scissors or blend.
  *Confirm before coding:* dump the guest blocks each `kRCxRegsIndirect` names -
  do the writes exist and we lose them, or does the guest bind through a packet
  we never decode? Record in an audit (Rule 04).

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
