# tools/game_runner — runtime execution harness

Owner: runtime/automation. Purpose: launch a retail title, observe what it
actually does, and produce a machine-readable record another session can trust.

## Front door

`session.py` is the supported entry point. Everything else in this directory is
a library it reuses.

```bash
python tools/game_runner/session.py list                       # titles + recent runs
python tools/game_runner/session.py kill                       # drop stale emulator processes
python tools/game_runner/session.py run --title PPSA02929 --duration 60
python tools/game_runner/session.py longrun --title PPSA02929 --minutes 30
python tools/game_runner/session.py show <run-id>
python tools/game_runner/session.py compare <run-id>           # vs baseline
python tools/game_runner/session.py baseline --update <run-id>
```

## Modules

| File | Role |
|---|---|
| `session.py` | Launch, monitor, sample frames, classify, record, compare to baseline |
| `screen_capture.py` | Window/HWND capture, perceptual frame hash, frame-diff ratio |
| `crash_analyzer.py` | Log-based crash classification, optional RIP disassembly |
| `input_harness.py` | Win32 `SendInput` keyboard injection |

Controller input is **not** reimplemented here. It goes through the emulator's
own replay mechanism: `--input <replay.json>` is forwarded to the CLI as
`--play-input=`, and replays are recorded with `pcsx5_cli --record-input=`.

## What a run record contains

`artifacts/runtime/<run-id>/record.json` (untracked), alongside `run.log`,
`frames/frame_NNNN.png` and any crash dump:

- identity — title, eboot, git revision, build revision (core DLL size + mtime), argv
- timing — start, end, requested vs actual duration, termination reason, exit code
- execution — `rendering`, `progressing`, first-frame time, frame count, unique
  frame hashes, mean/max frame change, longest freeze, last frame
- process — mean CPU, peak RSS
- markers — boot-progress markers, same vocabulary as `tools/autorun.py`
- crash — fatal signature, last RIP / module / thread, exception code

`last_rip` is the RIP from the last log line that carried one. The log
interleaves every guest thread, and handled exceptions log a RIP too, so treat
it as a starting point and confirm it against the fatal line before quoting it
as the crash address.

## Status vocabulary

A live process is not a running game. `status` is one of:

| Status | Meaning |
|---|---|
| `crashed` | Process exited and a fatal signature was found in the log |
| `exited` | Process exited with no fatal signature |
| `no-frame` | Process survived but never produced a capturable frame |
| `frozen` | Frames were produced but never changed — **not** stable execution |
| `progressing` | Frames were produced and consecutive frames differ |
| `ran-headless` | Headless run; frame validation was impossible by construction |

## Baseline

`tests/runtime_baseline.json` (tracked) holds one entry per title: boot success,
first-frame time, sustained runtime, crash location, current boundary and
regression state. `compare` answers "did this change improve or regress the
emulator?" from data rather than recollection. Update it deliberately, after a
run you trust, and record why in the owning walkthrough.

## Superseded modules

`runner.py` and `autonomous_loop.py` predate `session.py` and overlap it: both
launch the CLI and tail its output, neither distinguishes a rendering process
from a progressing one, and `autonomous_loop.py` duplicates the artifact-directory
layout. They are kept only because existing notes reference them.

**Removal condition:** delete both once no task or audit document in `docs/`
references them and `session.py` has covered a full regression cycle on both
titles. Do not add features to them — add to `session.py`.

`tools/autorun.py` and `tools/autopilot.py` are *not* superseded: they own the
build-fix-rerun search loop, which `session.py` deliberately does not do.
`session.py` observes a single run; those drive iteration.
