# Replay Files for Regression Testing

This directory stores controller input replay files (JSON) for the automated
regression test suite.  Each file records a sequence of controller inputs at
60 fps (vblank units) that exercises a specific game title.

## Naming Convention

```
<title_id>_<description>.json
```

Examples:
- `PPSA02929_menu.json` — menu navigation for Dreaming Sarah
- `PPSA10264_gameplay.json` — gameplay sequence for Jusant

The title ID is the first segment before the first underscore and is used to
map the replay to the corresponding game directory under `Games/<title_id>-app0/`.

## Replay Format

See `src/gpu/input/input_bot.h` for the canonical format definition:

```json
{
  "version": 1,
  "title_id": "PPSA02929",
  "events": [
    { "frame": 0,   "buttons": 0,    "lx": 128, "ly": 128, "rx": 128, "ry": 128, "l2": 0, "r2": 0 },
    { "frame": 120, "buttons": 1,    "lx": 128, "ly": 128, "rx": 128, "ry": 128, "l2": 0, "r2": 0 },
    { "frame": 240, "buttons": 0x40, "lx": 255, "ly": 128, "rx": 128, "ry": 128, "l2": 0, "r2": 0 }
  ]
}
```

Fields per event:
- `frame` — monotonic frame counter (60 Hz vblank units)
- `buttons` — bitmask of pressed buttons (see ControllerState::buttons)
- `lx`, `ly` — left stick (0-255, center 128)
- `rx`, `ry` — right stick (0-255, center 128)
- `l2`, `r2` — analog triggers (0-255)
- `touch` — number of touch contacts (optional, default 0)

## Recording Replays

Run the emulator with `--record-input=<path>` to capture live controller input:

```
pcsx5_cli.exe --record-input=replays/PPSA02929_menu.json Games/PPSA02929-app0/
```

The recorded file uses newline-delimited JSON format and is post-processed
into the standard array format on close.

## Running the Suite

```bash
# Run all replays and update the manifest
./tools/run_regression_suite.sh --update-manifest

# Run a single replay
./tools/run_bot_test.sh --play-input=replays/PPSA02929_menu.json Games/PPSA02929-app0/
```
