# WALKTHROUGH-2026-08-30-ui-05-playstation-pass

Completion of UI-05, the last task in
`docs/tasks/TASK-2026-08-30-ui-console-experience.md`.

## Scope

`src/ui_csharp/MainWindow.xaml`, `MainWindow.xaml.cs`. No emulator core change.

## What was actually left

UI-08 had already made tile focus unmistakable (scale, glow, opacity falloff).
What remained for UI-05 was **composition** — three specific faults visible in
the captured frames:

1. A **~180 px dead band** between the Play button and the shelf. The hero
   content was vertically centred inside a very tall row, so the screen read as
   two clusters with a hole between them.
2. The **executable path** — a full filesystem path — carried the same visual
   weight as the game's own metadata. That is developer detail, and it breaks the
   console illusion more than anything else on the screen.
3. Tiles were still modest for a 1080p console dashboard.

## Changes

The PS5 arrangement is: key art behind, the selected title and its action sitting
directly above a large row of game tiles. Anchoring the hero content to the
bottom of its row produces that, and removes the dead band as a *consequence*
rather than by padding it out.

| Element | Before | After |
|---|---|---|
| Hero content alignment | `Center` in a tall row | `Bottom`, 26 px above the shelf |
| Key art alignment | `Center` | `Bottom`, sharing the hero baseline |
| Shelf row | 340 px | 420 px |
| Tile strip height | 255 px | 320 px |
| Tile size | 200 px | 250 px |
| Focused tile scale | 1.14 | 1.10 |

Focus scale was *reduced* deliberately: at 250 px a 1.14 scale crowds the
neighbouring tile, and a larger base tile needs less scale to read as focused.

The executable path is now `TypeS`, dimmed to `#59A0A0A5`, single-line with
character ellipsis, and carries the full path as a tooltip. It stays available
for developers without shouting at the user.

A **bottom scrim** was added under the content — a vertical gradient from
transparent to near-opaque background colour. It keeps the title and metadata
legible over arbitrary key art, and makes the blurred upscale read as an
intentional backdrop rather than as a low-resolution image.

## Verification

```bash
dotnet build src/ui_csharp/Pcsx5Ui.csproj -c Release -r win-x64   # 0 warnings
python tools/check_ui_strings.py --baseline tests/ui_baseline.json
ctest --test-dir build -C Release --output-on-failure              # 46/46
python tools/validate_ai_framework.py                              # PASSED
```

Visual, from captured frames:

- Dead band gone; the screen now flows title → metadata → action → shelf.
- Tiles at 250 px with the focused tile scaled, fully opaque and glowing, while
  the unfocused tile sits at 0.55 opacity.
- Run `SHELL_20260830_150035_ui05-pass`.

**UI scale range** — the acceptance criterion explicitly required the layout to
hold across the configured scale range, so it was tested rather than assumed.
`UiScale` was set to **1.25** in the dev build's `config.ini`, the shell was run
and captured, and the config was restored afterwards. At 125 % nothing clips:
the hero, shelf, both tiles, the "View All" control and the footer hint bar all
remain fully visible. Run `SHELL_20260830_150149_ui05-scale125`.

## Result

`tests/ui_baseline.json` shows no regression: 7 hardcoded strings (the deliberate
brand-noun set), 273 `DynamicResource` references, locale parity intact across
all eleven files. CTest 46/46. `dist/` republished.

## Workstream status

All ten tasks in `TASK-2026-08-30-ui-console-experience.md` are complete:
UI-01 fullscreen, UI-02 in-app folder browser, UI-03 navigation completeness,
UI-04 unified hint bar, UI-05 PlayStation pass, UI-06 typography, UI-07 input and
controller-driven rebinding, UI-08 library overhaul, UI-09 Tools analyzer,
UI-10 hardcoded strings.

## Keyboard parity (follow-on)

Two of the user's Input-tab findings do **not** depend on the DualSense library,
so they were done ahead of that work: someone with no controller at all can now
configure and drive the shell.

### Assigning a binding from the keyboard

With a rebind armed, any key press assigns that key to the slot as a
`key_<name>` token; Escape cancels. Keyboard tokens are formatted rather than
given one locale key each (`bind.keyboard_fmt`), because the key space is
open-ended. This covers the stick-direction slots too, which is the
configuration half of "set up sticks from the keyboard".

**Verified with real keystrokes, no controller attached:** navigating to the
Input tab, focusing D-Pad Up, arming it and pressing `K` left the button reading
**"K  (Key)"** in green with the status line confirming the assignment.
Run `SHELL_20260830_150807_kb-bind`.

### Driving the shell from the keyboard

The shell previously had no keyboard navigation at all beyond F11. Arrows/WASD,
Enter/Space (Cross), Escape/Backspace (Circle), T (Triangle), F (Square) and Q/E
(tab left/right) now raise **virtual button flags that the input tick merges in**,
so keyboard and pad share one navigation implementation. That matters more than
the convenience: every screen gained keyboard support at once, and the two input
paths cannot drift apart as screens are added.

Keyboard directions bypass the hold-repeat model deliberately — a key press is a
discrete event, not a held stick. Keys are never stolen from a focused
`TextBox`, so typing in the search field still works.

**Verified:** two presses of `E` moved Library → Tools → Input, with the focus
ring landing on the first control of each screen (which also confirms UI-03's
focus seeding). Run `SHELL_20260830_150717_kb-nav`.

Slot names in the status line were also localized, so it reads "D-Pad Up is now
K (Key)" rather than the internal "PadUp".

## Automatic input-device detection

The shell now behaves like a PC game: prompts follow whichever device you touched
last, and **both stay live at all times** — nothing has to be selected.

A real key press sets the source to keyboard; genuine pad activity (any button,
a trigger past 60, or a stick past the 15000 deadzone — thresholds chosen so idle
stick drift cannot flap the source) sets it back. The legend re-renders
immediately rather than waiting for a screen change.

Rather than keeping two hint strings per screen, the hints became **device-neutral
templates** with glyph tokens substituted at render time:

```
%DEV% %TABS% Tabs  •  %DIR% Select Game  •  %OK% Play  •  %BACK% Stop
```

`%OK%` renders `[✕]` or `[Enter]`, `%BACK%` renders `[◯]` or `[Esc]`, `%DIR%`
renders `[D-Pad]` or `[Arrows]`, and so on. One string per screen instead of one
per screen per device, and a translator sees the sentence rather than button art.

**Verified:** on the Tools tab the legend read
`⌨ [Q/E] Tabs · [Arrows] Select Executable · [Enter] Analyze All` immediately
after a key press, having shown the controller glyphs before it.
Run `SHELL_20260830_151303_input-source`.

## Per-player device assignment — blocked on core work

The user also asked for Player 1 on a controller and Player 2 on the keyboard.
This was **specified rather than built**, because the shell cannot deliver it and
a settings control with nothing behind it would be worse than none. Two blockers,
both established by inspection:

1. **No input entry point exists in the native ABI.** `CoreBridge` exposes
   init/load/run/stop/pause/resume/shutdown/extract_pkg/get_last_error and
   nothing else. The core reads controllers itself; the shell cannot inject pad
   state for any player.
2. **The core accepts exactly one pad.** `src/hle/libpad.cpp` rejects
   `scePadOpen` unless `userId == SCE_PAD_PRIMARY_USER_ID` and `index == 0`, so a
   second controller is refused at open time.

Recorded as `docs/tasks/TASK-2026-08-30-multi-player-input-routing.md`, including
the architecture decision it turns on — whether input ownership moves to the
shell behind a new ABI entry point, or stays in the core driven by configuration.
That is a public-ABI change and a stopping condition under Rule 10, so it needs
explicit approval before implementation.

## Not in this workstream

The user's Input-tab hardware findings remain open and are queued behind the
DualSense work (see `docs/tasks/` and the reference implementation the user
selected, <https://github.com/Ohjurot/DualSense-Windows>):

- no live "test controller" panel on the Input tab;
- gamepad diagram highlight overlays misaligned with the buttons they represent;
- motor/vibration test, lightbar LED and microphone mute all non-functional;
- analog stick movement not registering;
- multiple-controller support untested.

Keyboard binding assignment and keyboard navigation are **done** (see above).
What remains of "simulate stick input from the keyboard" is the *runtime* half:
the shell can now map keys to stick-direction slots, but actually feeding
synthesized analog values to the guest happens in the native pad/HLE layer, not
in the shell. That is a core change and is deliberately out of this workstream's
scope; it should be specified as its own task rather than widened into a UI
change.
