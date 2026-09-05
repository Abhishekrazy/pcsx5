# TASK-2026-09-06 — Shell redesign: "Void console", with themes

## Scope

Implement the approved concept (design canvas "PCSX5 Console Concept",
direction A) in the WPF shell, screen by screen, on top of a new theme
system (ADR-003). The user's words: "start the design but make sure we're
not missing light/dark theme mode ... emulator colours also controllable in
settings as well as theme colours; C# as the top layer is fine."

The previous shell is not the base; it is the working version that keeps
shipping while each screen is replaced. Every replacement keeps the
behaviour the old screen had (pad navigation, keyboard, localization keys,
accessibility names) and adds nothing that Rule 12's ratchet would count.

## Prerequisites

- ADR-003 accepted (theme tokens; `Theme.cs`; settings fields).
- The concept canvas for reference: main, all games, input, settings, tools,
  console, error, pause overlay, quick settings, folder picker.

## Sequence (one buildable change each)

1. **Theme foundation** — token brushes in `App.xaml`; `Theme.cs`; config
   fields `ui.theme`, `ui.accent`, `ui.ground`; Settings entries (mode,
   accent swatches, hex); applied live. Verified by screenshots of the same
   screen in dark and light with two accents.
2. **Library (main)** — hero, five-title shelf, legend, on tokens.
3. **All games** — grid, search, sort, favourites (favourites need a
   per-title flag beside `recent_plays.json`).
4. **Settings** — side sections, rows with inline value cycling.
5. **Input** — device, motion, bindings grid, tests.
6. **Tools**, 7. **Console**, 8. **Error**, 9. **Pause overlay**,
10. **Quick settings**, 11. **Folder picker**.

## Acceptance

- Every screen renders correctly in dark and light, with a non-default
  accent, seen in screenshots cited from `artifacts/runtime/`.
- `python tools/check_ui_strings.py --baseline tests/ui_baseline.json`
  reports no regression after each step; `DynamicResource` count rises.
- Pad-only operation of every screen; keyboard equivalents shown in the
  legend; mouse still works.
- `TASKS.md` 4.13 tracks each step; falsified approaches are kept.

## Next task

After step 1: Library (main). The remaining steps follow in the order above
unless the user reorders them.
