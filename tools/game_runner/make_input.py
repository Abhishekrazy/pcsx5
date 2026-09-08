#!/usr/bin/env python3
"""Compile a readable input script into the emulator's JSON controller replay.

The replay format the core consumes (see src/gpu/input/input_bot.h) is a list of
absolute controller snapshots on a 60 Hz frame counter, each held until the next
one. Writing that by hand is error-prone: every press needs a matching release
event, and every timing is an absolute frame number. This compiles the obvious
thing instead:

    WAIT 5s
    PRESS CROSS 100ms
    WAIT 1s
    PRESS DOWN 100ms
    HOLD RIGHT 2s
    STICK LX 255 1s

Usage:
    python tools/game_runner/make_input.py script.txt out.json [--title PPSA02929]
    python tools/game_runner/make_input.py - out.json          # script on stdin

Commands (case-insensitive):
    WAIT <dur>              neutral for the duration
    PRESS <button> <dur>    press, hold for the duration, then release
    HOLD <button> <dur>     alias of PRESS (reads better for movement)
    STICK <axis> <v> <dur>  hold a stick axis at v (0..255) for the duration
                            axis is one of LX LY RX RY
    NEUTRAL <dur>           explicit neutral (same as WAIT)

Durations are `<n>s`, `<n>ms`, or a bare number of frames.
Buttons may be combined with '+', e.g. PRESS CROSS+DOWN 100ms.
Lines beginning with '#' are comments.
"""

import argparse
import json
import re
import sys

# Button bits, from the keyboard mapping in src/gpu/vulkan_backend.cpp.
BUTTONS = {
    "UP": 0x00000010,
    "DOWN": 0x00000040,
    "LEFT": 0x00000080,
    "RIGHT": 0x00000020,
    "CROSS": 0x00004000,
    "CIRCLE": 0x00002000,
    "SQUARE": 0x00008000,
    "TRIANGLE": 0x00001000,
    "L1": 0x00000400,
    "R1": 0x00000800,
    "L2": 0x00000100,
    "R2": 0x00000200,
    "OPTIONS": 0x00000008,
    "TOUCHPAD": 0x00100000,
}

AXES = {"LX": "lx", "LY": "ly", "RX": "rx", "RY": "ry"}

FRAMES_PER_SECOND = 60.0


def parse_duration(text):
    """Returns a whole number of 60 Hz frames, minimum 1."""
    m = re.fullmatch(r"(\d+(?:\.\d+)?)(ms|s|f)?", text.strip(), re.IGNORECASE)
    if not m:
        raise ValueError("bad duration %r (expected 5s, 100ms or a frame count)" % text)
    value = float(m.group(1))
    unit = (m.group(2) or "f").lower()
    if unit == "s":
        frames = value * FRAMES_PER_SECOND
    elif unit == "ms":
        frames = value * FRAMES_PER_SECOND / 1000.0
    else:
        frames = value
    return max(1, int(round(frames)))


def parse_buttons(text):
    mask = 0
    for name in text.split("+"):
        key = name.strip().upper()
        if key not in BUTTONS:
            raise ValueError("unknown button %r (known: %s)"
                             % (name, ", ".join(sorted(BUTTONS))))
        mask |= BUTTONS[key]
    return mask


def neutral_event(frame):
    return {"frame": frame, "buttons": 0,
            "lx": 128, "ly": 128, "rx": 128, "ry": 128, "l2": 0, "r2": 0}


def compile_script(lines):
    """Returns the replay event list. Emits a snapshot only when state changes,
    since the bot holds each event until the next one."""
    events = [neutral_event(0)]
    frame = 0

    def emit(frame_no, buttons=0, axes=None):
        event = neutral_event(frame_no)
        event["buttons"] = buttons
        if axes:
            event.update(axes)
        # A repeat of the previous state carries no information.
        prev = events[-1]
        if all(prev[k] == event[k] for k in event if k != "frame"):
            return
        events.append(event)

    for lineno, raw in enumerate(lines, 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        op = parts[0].upper()
        try:
            if op in ("WAIT", "NEUTRAL"):
                emit(frame)
                frame += parse_duration(parts[1])
            elif op in ("PRESS", "HOLD"):
                mask = parse_buttons(parts[1])
                held = parse_duration(parts[2])
                emit(frame, buttons=mask)
                frame += held
                emit(frame)
            elif op == "STICK":
                axis = AXES[parts[1].upper()]
                value = int(parts[2])
                if not 0 <= value <= 255:
                    raise ValueError("stick value %d out of range 0..255" % value)
                held = parse_duration(parts[3])
                emit(frame, axes={axis: value})
                frame += held
                emit(frame)
            else:
                raise ValueError("unknown command %r" % parts[0])
        except (IndexError, KeyError, ValueError) as exc:
            raise SystemExit("line %d: %s\n  %s" % (lineno, exc, raw.rstrip()))

    # Always finish neutral so nothing is left held down.
    emit(frame, buttons=0)
    return events


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("script", help="input script, or - for stdin")
    ap.add_argument("out", help="replay JSON to write")
    ap.add_argument("--title", default="", help="title id recorded in the replay")
    args = ap.parse_args()

    if args.script == "-":
        lines = sys.stdin.read().splitlines()
    else:
        with open(args.script, encoding="utf-8") as handle:
            lines = handle.read().splitlines()

    events = compile_script(lines)
    replay = {"version": 1, "title_id": args.title, "events": events}
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(replay, handle, indent=2)
        handle.write("\n")

    last = events[-1]["frame"]
    print("%s: %d events, %d frames (%.1fs at 60 Hz)"
          % (args.out, len(events), last, last / FRAMES_PER_SECOND))


if __name__ == "__main__":
    main()
