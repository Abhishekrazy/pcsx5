# WALKTHROUGH 2026-09-08 - Interactive rendering stabilization

How the work in `TASK-2026-09-08-interactive-rendering-stabilization.md` was
carried out and verified. Evidence and reasoning are in
`docs/audits/AUDIT-2026-09-08-frame-rate-collapse.md`.

## Environment

```
Commit at start        003117b
Toolchain              Visual Studio 18 2026, x64
GPU                    NVIDIA GeForce RTX 5070 Ti, driver 616.56
Vulkan                 device 1.4.351, instance requesting 1.2
Host-pointer import    available, minImportedHostPointerAlignment = 4096
```

## Commands

Build and test:

```bash
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Run the title, and read the frame rate:

```bash
python tools/game_runner/session.py run --title PPSA02929 --duration 60
d=artifacts/runtime/$(ls -t artifacts/runtime | head -1)
grep -o "FPS: [0-9.]*" $d/run.log | tail -10
```

The `FPS:` line is the metric to use. Do **not** count flips in the run log:
three identical runs gave 482, 394 and 483, which decides nothing.

List titles, and survey them:

```bash
python tools/game_runner/session.py list
for t in PPSA01668 PPSA10112 PPSA15552 PPSA20591 PPSA10264; do
  timeout 130 python tools/game_runner/session.py run --title $t --duration 45
done
```

## Sequence

1. **Established a trustworthy metric.** Replaced flip counting with the
   existing `FPS:` line, cross-checked against the guest's own fixed-timestep
   counter (`1.7333 / 0.016666` = exactly 104 ticks of 1/60 s in a 120 s run).

2. **Profiled by opcode.** `op=0x35` (`DRAW_INDEX_OFFSET_2`) accounted for
   39.7 s of a 60 s run across 1174 packets - 33.8 ms each, over 200x the next
   opcode.

3. **Profiled the draw executor in five phases.** Storage-buffer upload was
   22.8 s of 26.9 s over 800 draws.

4. **Measured per-bind volume.** At t=14 s the mean bound range grows about 5x
   (105 KB to 530 KB, max 256 KB to 1 MB) - exactly where the frame rate starts
   falling.

5. **Four attempts at the copy**, three reverted. See the audit.

6. **Implemented host-pointer import.** Storage phase 21.0 s to 0.20 s.

7. **Re-profiled.** The bottleneck moved out of the draw path: GPU-side work
   fell to ~3.4 s of a 60 s run.

8. **Found the HLE dispatch cost.** 2,262,337 calls per run; the dispatcher
   copied the whole symbol, built strings for a disabled log, copied more into
   a trace ring, and took a global mutex to rewrite constant fields.

9. **Split the per-draw cost inside `AgcExecuteDraw`.** Over 2400 draws:
   decode 62 ms, **scalar evaluation 7,621 ms**, descriptor setup 54 ms,
   Vulkan execute 2,452 ms.

10. **Tried memoizing scalar evaluation, and rejected it** - it bought 4.6 to
    6.9 fps and lost unique frames, 20 down to 16. Isolated by disabling only
    the cache lookup and re-running.

11. **Surveyed the other five titles.** All crash during boot.

## Instrumentation discipline

All instrumentation in this task was temporary and removed. Two notes for
whoever repeats this:

- **Do not instrument a hot path with shared atomics.** A per-call timer on
  `HleDispatch` using two shared `std::atomic` counters reported 157
  microseconds per call. Re-run on the fixed build it still reported 137-220,
  which is how the artifact was caught: it was measuring its own contention.
  Use per-thread counters aggregated at exit.
- **Do not `git checkout` a file to strip instrumentation from it** if that
  file also carries your fix. This happened twice here and silently reverted
  the import. Save `git diff` to a patch first, or strip the instrumentation
  by targeted edit.

## Results

| | before | after |
|---|---|---|
| Steady-state fps | 1.0 | **4.4 - 4.7** |
| Storage-buffer phase, 1200 draws | 21.0 s | **0.20 s** |
| GPU-side draw work, per 60 s run | dominant | ~3.4 s, about 6% |
| Draw calls per frame | - | 14 to 18 (measured; not a bottleneck) |
| Tests | 54 of 54 | **54 of 54** |
| Vulkan validation errors | 0 | **0** |
| Guarded write faults | 0 | **0** |
| PPSA02929 | `progressing` | `progressing` |

## Visual state

The title screen renders legibly - "Sarah" is clean where it was previously a
checkerboard. The upper region still shows the blocky artifact recorded in
`AUDIT-2026-09-07-artifact-localization.md`, and glyph edges smear. The
checkerboard cause is not proven and the improvement is not claimed as a fix.

## Baseline decision

`tests/runtime_baseline.json` was **not** updated. Runs classify `frozen` about
half the time with 16 to 22 of 29 unique frames, and the classifier itself
reports the status was not stable across its baseline samples. Whether that is
the title reaching a static menu or a real stall is unresolved, and a baseline
should not be moved onto an unexplained classification.

## Not done

Phases 4, 7 and 9 of the mission brief were not started; phase 12 reached 120 s
rather than 30 or 60 minutes. No watchdog was added. These remain open in
`TASKS.md`.
