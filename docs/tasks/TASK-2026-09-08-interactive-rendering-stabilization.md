# TASK 2026-09-08 - Interactive rendering, presentation, input and runtime stabilization

## Objective

Take PPSA02929 from "boots and renders a partially recognizable intro" to
stable interactive 2D menu execution: correct presentation, usable input, and
sustainable performance - without hardcoding frames, bypassing guest execution,
or weakening tests.

## Scope of this iteration

This document covers the first pass. It is deliberately narrower than the full
mission brief, because the change budget is one architectural concern per
iteration and because two of the mission's phases turned out to be already
satisfied by existing tooling.

| Mission phase | State after this iteration |
|---|---|
| 0 - baseline | Done. Reproducible metric established (see below). |
| 1 - observation harness | **Already existed.** `tools/game_runner/session.py` launches titles, captures timestamped logs to `artifacts/runtime/<run-id>/`, screenshots at intervals, hashes frames, classifies runs, and compares against a baseline. No new harness was written; writing one would have duplicated it. |
| 2 - input harness | **Partly existed.** `session.py run --input <script.json>`, `--clicks`, `--keys`. Not exercised in this iteration. |
| 3 - checkerboard text | Investigated, not resolved. Localized to a known pass. |
| 4 - motion blur | Not started. |
| 5 - 4 FPS profile | **Done, root causes found and two fixed.** |
| 6 - menu hang | Root cause identified as the same cost as phase 5. No watchdog added. |
| 7 - input recovery | Not started. |
| 8 - GPU/presentation stability | Partly - see the import work. |
| 9 - automated interactive test | Not started. |
| 10 - regression protection | Existing suite kept green throughout (54 of 54). |
| 11 - control title | PPSA02929 is the affected title and the control; no regression. |
| 12 - long-run stability | Partial - 120 s runs, not 30 or 60 minutes. |
| 13 - architectural boundary | One recorded (see below). |
| 14 - documentation | This document, the audit, and the walkthrough. |

## Correction to the brief's document paths

The brief asks for a phase-progress document plus a runtime-lifecycle and a
boot-pipeline document, all under the docs directory. No such paths exist here.
The lifecycle and boot documents live in `architecture/RUNTIME_LIFECYCLE.md`
and `architecture/PS5_BOOT_PIPELINE.md`, and this repository has no
phase-progress document at all. Rather than invent a parallel history, this
task records progress in `TASKS.md` and the audit, which is where this project
actually tracks it.

(The `doc_links` test enforces exactly this: a document that cites a path which
does not resolve fails the build. Quoting the brief's paths verbatim tripped
it, which is the test doing its job.)

## Baseline

| | |
|---|---|
| Commit at start | `003117b` |
| Build | `cmake --build build --config Release`, Visual Studio 18 2026, x64 |
| GPU | NVIDIA GeForce RTX 5070 Ti, driver 616.56 |
| Tests | 54 of 54 |
| PPSA02929 steady-state frame rate | **1.0 fps** |
| Classification | `progressing`, 29 of 29 unique frames |

The frame-rate metric is the `FPS:` line that `core_api.cpp` already emits once
a second via `Diagnostics::LogFrameTimingStats`. It agrees with the guest's own
fixed-timestep frame counter to within rounding. Earlier phases counted flips
in the run log; that proxy is unusable, giving 482, 394 and 483 across three
identical runs.

## Acceptance criteria

1. The 4 fps condition has a **measured** root cause, and is corrected as far
   as the architecture permits without trading correctness for speed.
2. Rendering corruption is either fixed or localized to a documented boundary.
3. The menu delay has a measured cause.
4. No test is weakened; the suite stays green.
5. PPSA02929 does not regress.
6. Every claim carries an evidence label (Rule 02).

## Outcome against those criteria

1. **Met.** Three independent causes measured; two fixed, one rejected on
   correctness grounds. 1.0 fps to a stable 4.4-4.7.
2. **Partly met.** The title text renders legibly where it was previously
   checkerboarded, but the upper-region blocks and glyph smearing remain, and
   the checkerboard cause is not proven.
3. **Met.** The delay is the same per-draw cost as (1), not an input or
   synchronization stall. Presentation was excluded by measurement.
4. **Met.** 54 of 54 throughout, nothing skipped or relaxed.
5. **Met.** PPSA02929 is the affected title; it improved and did not regress.
6. **Met**, including four falsified hypotheses and three corrections to my own
   earlier claims, all recorded.

## Work delivered

**Zero-copy guest buffer binding.** `VK_EXT_external_memory_host` backs a
`VkBuffer` directly with the guest's own pages, removing the per-draw copy
entirely rather than trying to prove it unnecessary. 84% of binds and 72% of
bytes; storage-buffer phase 21.0 s to 0.20 s over 1200 draws.

**HLE dispatch bookkeeping.** Roughly six heap allocations per dispatch and a
global exclusive mutex, on a path taken 2,262,337 times per 60 s run. Removed.

Details, evidence and the four falsified hypotheses are in
`docs/audits/AUDIT-2026-09-08-frame-rate-collapse.md`.

## Recorded boundary

**Scalar evaluation cannot be memoized on the user-data bank.** Shader scalar
evaluation is 75% of remaining draw time (3.2 ms per draw). Memoizing it on
(shader address, user SGPR bank) raised the frame rate from 4.6 to 6.9 fps -
and cost unique frames, 20 down to 16, because the guest rewrites its
descriptor table *in place*: the table contents change while the user data that
points at it does not, so the key never moves and the cache serves stale
bindings. Reverted. This is `SOFT BOUNDARY`, not hard: the correct fix is to
split evaluation into the part that locates descriptors (pure per shader and
bank, cacheable) and the part that reads their current values (cheap, per
draw). That requires a change inside `GcnEvaluateScalarState` and belongs in
its own iteration.

## Next task

1. Split scalar evaluation as above - the largest remaining measured cost.
2. Settle whether the `frozen` classification reflects a static menu or a real
   stall, before it is read as either.
3. The five other titles under `Games/` all crash during boot, with four
   distinct signatures. See `TASKS.md`.
