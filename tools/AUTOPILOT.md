# tools/AUTOPILOT.md — Dreaming Sarah (PPSA02929) Python autopilot

`autopilot.py` owns the whole bring-up cycle so **Python does all repetition**:

    build -> run(classify + score) -> try a fix/knob -> rebuild -> rerun ...
    until content-load advances into menus/in-game, then drive it with an input
    replay ("auto play by guessing") via hill-climbing over generated controller
    sequences. Every source/config edit is **byte-exact reversible**: an improvement
    is kept and persisted; a regression is reverted automatically.

Pure Python stdlib only (no torch/capstone), so this driver starts instantly and
never breaks on the GPU brute-force stack. It drives the C++ emulator through its
own CLI (`pcsx5_cli.exe`), not the GPU backends that are irrelevant to boot/load.

## Subcommands

```bash
# One build + headless run, full report (outcome/markers/states/BUG signatures).
python tools/autopilot.py baseline [--trace] [--probe]

# Closed fix->rerun loop up to N rounds, then replay hill-climb auto-play once the
# game reaches content-load. Bounded rebuilds: build() runs only when source/config
# actually changed since the last run (kept patches force one rebuild).
python tools/autopilot.py iterate --max-rounds 6 --play-attempts 8 [--trace] [--probe]

# One-off manual, reversible config toggle of a single global.json field.
python tools/autopilot.py knob <field> <old> <new>
```

State lives under `.work/`: `autopilot_history.jsonl` (per-run verdicts),
`autopilot_state.json` (applied patches, best run/replay, reached_content flag).
Logs: `.work/autopilot_logs/run_*.log`.

Reversibility does NOT depend on git being present (git is not installed in this
harness); it depends on `apply_edit(path, from_text, to_text)` which takes the EXACT
on-disk text as `from` and returns an inverse op that byte-for-byte restores it. Every
source/config edit records its inverse and reverts automatically on regression — so no
hand-edit ever persists a bad state, whether or not the repo is under VCS control.

## What it fixes / searches for

- **Curated code-fix registry** (`PATCHES`) — keyed to observed log signatures and
  only applied when one matches (or no untried patch remains):
  - `tls-precommit-relocate`: raise pre-commit base off the eboot slot
    (`src/memory/memory.cpp::pre_base`).
  - `strict-imports`: enable strict HLE dylib imports (SharpEmu parity) in global.json.
  Each is applied -> build -> run; **kept only if it strictly improves the score**,
  otherwise reverted and dropped from state.

- **Safe config knobs** (`_knob_field_names`) — toggled in `pcsx5_config/global.json`
  with exact-text round-trips so each trial reverts cleanly: currently
  `audio.backend` (0/1/2) and `graphics.renderer` (0/1).

- **Replay auto-play** ("guessing"): once content-load is reached, hill-climbs
  generated controller replays (`--play-input`) to push Dreaming Sarah deeper toward
  menu/in-game. Boot phase is input-free; only the tail explores button combos and
  mutates timings; it stops on menu/ingame or a plateau. The best replay is saved to
  `replays/PPSA02929_autopilot.json` for a human to refine with `--record-input`.

## Current blocker (important)

The **Construct runtime data.js parse crash** (`std::invalid_argument("parse error -
unexpected ...")`) at content-load is NOT addressable by config/knobs/replays alone —
it needs a C++ fix in the HLE JSON/Construct parser. Evidence:

- Every logged Dreaming Sarah run ends `GUEST APPLICATION CRASHED` / `VEH Unhandled
  Exception`; no log ever reaches a content-load/menu marker (`reached_content` stays
  False, so auto-play never starts — by design).
- The reproduction lives in **`src/hle/liblibc.cpp` (~L1620–1735)**: the guest's
  multi-threaded `P.Worker` pool throws on a Construct JSON read; comments attribute
  it to "the C2 runtime's data.js chunk reader died" + a P.Worker race. Config knobs
  (`audio.backend`, `graphics.renderer`) and curated patches can't clear this — only
  an HLE parser fix advances past content-load.

`classify()` records it as a `construct-parse` BUG signature; once a C++ fix exists,
add a curated PATCH whose `when:` matches the crash and whose edits patch liblibc.cpp's
JSON reader to be tolerant (skip/truncate safely instead of throwing), then rerun —
the autopilot keeps it only if post-fix runs advance deeper.

## Adding a new curated patch or knob

- **Curated code fix**: append an entry to `PATCHES`: `{key,label,when:[regex...],
  edits:[(relpath, "old_text", "new_text")]}`. `when` gates it to the log signature
  that motivated it; apply/revert are exact-text so double-edits are no-ops.
- **Config knob**: add a tuple to `_knob_field_names()` as `(dotted.field, current,
  [try_values...])`; edits go through `set_json_field` (nested-aware) + reversible
  apply_edit.
