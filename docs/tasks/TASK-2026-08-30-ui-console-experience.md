# TASK-2026-08-30-ui-console-experience

Make the PCSX5 desktop shell a console experience: fully operable from a
PlayStation controller from launch, starting fullscreen, with no Windows chrome
breaking the illusion, presented as a PlayStation 5 interface with
PlayStation 4-style on-screen button hints.

## Owner

Desktop shell (`src/ui_csharp/`). No emulator core change is in scope.

## Prerequisites

- Rule 11 (UI shell boundary) and Rule 12 (UX / accessibility / localization).
- Skills: `ui-change`, `ui-verify`, `ui-audit`.
- Baselines: `tests/ui_baseline.json`, `tests/runtime_baseline.json`.

## Boundary scope

In scope: `src/ui_csharp/**`, `assets/lang/*.json`.

Out of scope: anything under `src/` native subsystems, the `CoreBridge` exported
signatures, and the emulator boundary work on PPSA02929/PPSA21564. If a UI need
appears to require a core change, stop and record it (Rule 10) rather than
widening this task.

## Current state (measured, commit 66e0369)

Gamepad support is **not** absent — it is substantial and uneven:

- `ControllerTimer_Tick` (~330 lines) polls DualSense via `WindowsDualSenseReader`
  with an XInput fallback, does button-edge detection, analog-stick navigation
  with a 220 ms cooldown, and routes to per-screen handlers.
- Per-screen handlers exist for: pause menu, watchdog toast, folder picker
  prompt, settings, controller setup, analyzer, logs.
- Shortcut combos exist: force stop (PS+Options / PS+L1+R1 / PS+L3+R3), console
  drawer (L3+R3 / Share+Triangle), PS button home/graceful-stop.
- `FooterGamepadHints` is assigned ad hoc in 12 places.

The gaps are specific:

| Gap | Evidence |
|---|---|
| No fullscreen start | No startup window-state code; `UiSection` has only `language`, `title_music_enabled`, `scale` |
| Folder picking escapes to Windows | `FolderPickerOpen_Click` (line ~1593), `BtnAddFolder_Click` (~3092) and `FirstRunBrowse_Click` (~3133) all call `Microsoft.Win32.OpenFolderDialog`, which no controller can drive. `FolderPickerOverlay` is only a prompt in front of it |
| Hints are ad hoc | 12 scattered `FooterGamepadHints.Text = ...` assignments, no per-screen model |
| Accessibility absent | 0 of 119 interactive controls carry `AutomationProperties.Name` |
| Not themeable | 0 `DynamicResource` vs 108 `StaticResource` |
| Not localizable | 229 hardcoded user-visible strings in XAML |

## Tasks

### UI-01 — Fullscreen-first startup

Start the shell fullscreen (borderless), with a persisted preference and a
toggle available from keyboard (F11) and controller.

**Acceptance:** shell launches fullscreen on a clean config; toggling to windowed
and back works from both input paths; the preference survives a restart; the
embedded emulator window (`EmbedEmulatorWindow` / `ResizeEmbeddedWindow`) still
sizes correctly in both modes.

### UI-02 — In-app PS5-style folder browser

Replace every `Microsoft.Win32.OpenFolderDialog` use with a real in-app directory
browser: drive/root list, breadcrumb, scrollable folder list, select, cancel.
Keep the existing `_folderPickerTarget` dispatch (`firstrun`, `settings`).

**Acceptance:** a game folder can be added start-to-finish with **only** a
controller; the same flow works with keyboard and with mouse; no Windows dialog
appears anywhere in the flow; a folder with no subdirectories, a permission-denied
directory, and a removed drive are all handled without an unhandled exception.

### UI-03 — Gamepad navigation completeness

Audit every screen for reachability. Give focus a visible, consistent treatment.
Set sensible initial focus per screen so a controller user is never stranded with
nothing focused. Close the gaps the audit finds (search field, Tools, Settings
detail pages, Controller Setup, dialogs).

**Acceptance:** from launch, every screen and every interactive control is
reachable and operable by controller alone; focus is always visible; no screen
can be entered without a way back; keyboard and mouse remain equal.

### UI-04 — PlayStation 4-style contextual hint bar

Replace the 12 ad-hoc `FooterGamepadHints.Text` assignments with one hint model
that each screen declares, rendered as a persistent legend in the PS4 style the
user prefers.

**Acceptance:** every screen shows the actions actually available on it; hints
update on screen and state change; there is exactly one place that renders them;
adding a screen without hints is obvious.

### UI-05 — PlayStation 5 interface pass

Visual alignment with the PS5 interface: home row of game tiles, hero area,
focus scale/glow on tiles, PS5 typography and spacing.

**Acceptance:** the library reads as a console home screen; tile focus is
unmistakable at TV viewing distance; layout holds at 1080p and at the configured
UI scale range; no regression in `tests/ui_baseline.json`.

### UI-06 — Typography and readability

Text is too small throughout, button labels especially. Measured: **177 of 252**
`FontSize` attributes in `MainWindow.xaml` are 9, 10 or 11 px — sizes intended
for a compact desktop tool, not a fullscreen console dashboard viewed at
distance.

Introduce a named type ramp in `App.xaml` and reference it via `DynamicResource`
so sizes are tunable in one place, and raise the base of the ramp.

**Acceptance:** no user-visible text below 12 px; every screen re-checked for
clipping and overflow at 1080p and across the UI scale range; the ramp is
referenced by resource, not by literal; `DynamicResource` count rises.

### UI-07 — Input screen overhaul and controller-driven rebinding

The Controller Setup screen is visually cluttered and, by the user's report, not
properly operable with the controller it configures. Rebinding infrastructure
partly exists (`BtnMap_Click`, `_activeRebindBtn`, capture at ~line 3523) but is
not discoverable or reliably reachable by pad.

**Acceptance:** the whole screen is navigable by controller; a binding can be
chosen and set **using the controller itself** — highlight a binding, press to
arm, press the desired button to bind, with a visible armed state and a cancel
path; the layout is legible rather than dense; keyboard and mouse still work.

### UI-08 — Library screen overhaul

The library is the front door and currently does not read as a console home
screen.

**Acceptance:** reads as a PS5 home screen; tile focus is unmistakable at
distance; the hero area, metadata and tile row are visually balanced; layout
holds at 1080p and across the UI scale range.

### UI-09 — Fix the Tools tab

**Root cause found and verified:** `RunBootAnalyzer()` passes `game.EbootPath`
(a file) to `pcsx5_boot_parser.exe`, which expects the game **directory**. Given
a file it prints its usage banner and exits, so the grid stays empty and the log
sticks at "Running analysis on all games...".

A second, latent fault: `LocateBootParser()` probes a fixed list of relative
paths. The dev build happens to match one; the packaged `dist` layout does not,
so the Tools tab would fail there even once the argument is fixed. Same class of
defect as the locale-path bug fixed in UI-01/02 — the app should carry its tools,
not guess where they live.

**Acceptance:** analysis populates the grid for both titles; the parser is
resolved in both dev and packaged layouts; a missing parser reports a clear,
localized message rather than an empty screen.

### UI-10 — Reduce hardcoded strings

Hardcoded strings are **not** mandatory — 224 is accumulated debt. The ratchet
stopped it growing; this task starts paying it down.

Migrate user-visible strings from XAML literals to `I18n`, working through the
screens touched by UI-06 to UI-08 so the work rides along with changes already
being verified. Proper nouns ("PCSX5", "DualSense") legitimately stay literal.

**Acceptance:** the hardcoded count falls substantially; every migrated string
has a key in all eleven locale files; no screen shows a raw key at runtime
(verified by `ui-verify`, which is exactly how the locale-loading bug was found).

## Cross-cutting requirements

Every task above must, per Rule 12:

- add no new hardcoded user-visible strings — new text goes through `I18n` with
  the key added to all ten locale files;
- give every new interactive control an `AutomationProperties.Name`;
- reference brushes via `DynamicResource` and define new colours in `App.xaml`;
- keep keyboard and mouse working as equal input paths;
- end with a `ui-verify` run and a screenshot of the affected screen.

## Verification

Per task:

```bash
dotnet build src/ui_csharp/Pcsx5Ui.csproj -c Release -r win-x64
python tools/check_ui_strings.py --baseline tests/ui_baseline.json
python tools/game_runner/session.py shell --duration 45 --label <task>
```

The emulator must be unaffected: `ctest --test-dir build -C Release` stays green
and `tests/runtime_baseline.json` does not move.

## Sequence and rationale

UI-01 → UI-02 → UI-03 → UI-04 → UI-05.

UI-02 is the highest-value item: it is the only gap that *hard-blocks*
controller-only use, because a Windows dialog cannot be driven by a gamepad at
all. UI-01 precedes it only because it is small and changes the window model that
the folder browser overlay renders inside.

## Next task after this workstream

Return to the emulator boundary work recorded in
`docs/walkthroughs/WALKTHROUGH-2026-08-30-ai-framework-initialization.md`:
PPSA02929's fatal fault site.
