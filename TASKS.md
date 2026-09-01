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

## Now

- [ ] **Execute every targetless draw, not just the last one**
  `AgcExecuteDraw` stashes a draw with no colour target in `st.pending_targetless`,
  a **single slot**, so every earlier draw in the frame is overwritten and lost;
  only the last is replayed at the flip. Measured loss: **87–88% of all guest
  draws** across two independent runs (2995 submitted / 372 executed; 2715 / 352).
  Blast radius: any title whose draws are classified targetless.
  - [ ] Replace the single slot with an ordered queue of owned calls
  - [ ] Replay the queue in submission order against the buffer the flip names
  - [ ] Test: a submit carrying N targetless draws executes N, not 1
  *Done when:* the executed/dropped counters show no drops for a normal frame,
  and a test fails before and passes after.

- [ ] **Recover why the colour-target binding is never observed**
  `DecodeRenderTarget` returns false on every draw of the run: CB_COLOR0
  registers `0x318`/`0x319`/`0x31C`/`0x3B0` appear in **zero** of 784 captured
  submits, and 2477 indirect context-register writes per frame collapse to 53
  distinct registers — no viewport, no scissors, no blend. Either
  `ApplySubmittedRegisters` is losing the writes, or the guest binds its target
  through a packet the walker does not decode.
  *Confirm before coding:* dump the guest blocks each `kRCxRegsIndirect` packet
  names and answer one binary question — does any entry target `0x318`/`0x319`/
  `0x31C`/`0x3B0`? Record the recovered layout in an audit (Rule 04).

---

## Next

- [ ] **Synthetic / keyboard input path in the core**
  There is no keyboard input anywhere in `src/` — no `GetAsyncKeyState`, no
  `WM_KEYDOWN`. Pad state comes only from physical controllers, so any title
  waiting for a button cannot be satisfied without hardware, and **no automated
  test can drive input at all**. Whole-project blast radius: unblocks input
  regression testing and users without a DualSense.
  - [ ] Synthetic pad state injectable from the harness
  - [ ] Keyboard bindings, including analog sticks simulated from keys
  - [ ] Test: a scripted button press reaches `scePadReadState`

- [ ] **A guest-filesystem test fixture**
  Several HLE implementations cannot be tested because exercising them needs a
  mounted guest filesystem with real descriptors. This blocks tests for
  `feof`/`fgets`/`fgetc` and for the guarded-transfer reporters.

- [ ] **Execute or explain the AGC packets the walker skips**
  Submitted by the guest, not executed: NOP sub-op `0x19` (DMA data),
  `IT_EVENT_WRITE` (`0x46`, logged only), op `0x76`.

- [ ] **Save-data mount returns an empty mount point**
  The guest builds `/-saveindex` with no prefix, and `sceSaveDataSetParam` /
  `sceSaveDataSaveIcon` are unimplemented stubs.

- [ ] **Performance: ~9 fps against a reference emulator's 60 fps on the same dump**
  Measure honestly first — flip rate is not uniform across a run, so
  flips ÷ duration compares nothing between runs of different lengths. Needs
  flips counted in a fixed window after a fixed marker.

## Later

- [ ] **DualSense features confirmed broken against real hardware** — vibration,
  lightbar, mic-mute, analog sticks not registering, misaligned diagram
  overlays, no live test panel. Needs the user's hardware to verify. Queued
  behind the synthetic input path.
- [ ] **Multiple controllers** — never exercised.
- [ ] **UI ratchet** — 229 hardcoded XAML strings, 0 of 119 interactive controls
  with `AutomationProperties.Name`, 0 `DynamicResource` references.
- [ ] **Committed build outputs** — `src/ui_csharp/bin/**` contains three copies
  of `pcsx5_core.dll`. CLAUDE.md forbids committing build outputs.
- [ ] **`tools/dream_tool.py`** is named for Dreaming Sarah but hardcodes a
  different title's eboot path.
- [ ] **`GetWindowRect` includes the DWM invisible resize border** (~7px), so the
  harness's screen-grab fallback frames a few foreign pixels.
  `DWMWA_EXTENDED_FRAME_BOUNDS` is the correct source.

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
