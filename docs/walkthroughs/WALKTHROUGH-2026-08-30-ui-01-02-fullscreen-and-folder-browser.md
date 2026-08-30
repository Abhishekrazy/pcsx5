# WALKTHROUGH-2026-08-30-ui-01-02-fullscreen-and-folder-browser

Completion of UI-01 and UI-02 from
`docs/tasks/TASK-2026-08-30-ui-console-experience.md`, plus two defects found
along the way.

## Scope

`src/ui_csharp/**`, `assets/lang/*.json`, and the verification harness. No
emulator core change: `ctest --test-dir build -C Release` stayed at 46/46 and
`tests/runtime_baseline.json` was not touched.

## UI-01 — Fullscreen-first startup

The shell now starts fullscreen and the preference persists.

- `UiSection.start_fullscreen` (default `true`), persisted as `Ui/StartFullscreen`.
  Kept **separate** from the existing `graphics.fullscreen`, which governs the
  *game* window, not the dashboard — collapsing the two would have conflated two
  different things.
- `ApplyShellFullscreen(bool)` sets `WindowStyle=None` + `WindowState=Maximized`
  (which covers the taskbar) and collapses the `WindowChrome` caption and resize
  bands so the top 50 px is not a drag handle with no title bar drawn in it.
  It cycles through `Normal` so WPF re-applies the style to an already-maximized
  window, and calls `ResizeEmbeddedWindow()` so an embedded game window follows.
- Toggle paths: **F11** (new `Window_PreviewKeyDown`), the in-app maximize button
  (which now exits fullscreen rather than fighting it), and a new
  **System → UI & Personalization → Start Interface in Fullscreen** toggle, which
  is reachable by controller through the existing `HandleSettingsGamepadNav`.

There is deliberately **no new gamepad chord** for fullscreen. A console has no
fullscreen concept, so inventing a combo would have added a shortcut the user did
not ask for; the Settings toggle already satisfies controller-only operation.

**Verified:** captured frames measure 1920x1080 against a 1920x1080 screen, with
no OS title bar and the taskbar covered.
Run `SHELL_20260830_130246_ui01-fullscreen`.

## UI-02 — In-app PS5-style folder browser

`Microsoft.Win32.OpenFolderDialog` is gone from the shell — the count went from
**3 call sites to 0**. A Windows dialog cannot be driven by a controller at all,
so it was the only thing that *hard-blocked* controller-only use.

The old `FolderPickerOverlay` was a prompt whose single button opened the Windows
dialog. It is now a real browser:

- Drive list with volume labels, from `DriveInfo.GetDrives()` filtered to
  `IsReady` so an empty card reader or removed drive is skipped.
- Breadcrumb path, parent entry, scrollable folder list.
- Buttons: Up / Open / Select This Folder / Cancel. "Select This Folder" is
  disabled while the drive list is shown, because selecting "This PC" is
  meaningless.
- **Gamepad:** D-Pad move (wrapping), Cross open, Triangle select, Circle up —
  and Circle at the drive list cancels.
- **Keyboard:** the ListBox and buttons are focusable and behave natively.
- **Mouse:** click to highlight, double-click to open.
- Errors are handled per-entry: `UnauthorizedAccessException` renders an "Access
  denied" row rather than throwing, other IO failures render "Folder unavailable",
  an empty directory renders "No subfolders here", and a drive that vanishes
  between enumeration and query is skipped.

All three former dialog sites now route through it, including the Settings
sub-page one, which keeps its `PopulateSubPage` refresh via a captured
`_folderPickerRefreshKey`.

**Verified end to end by driving the real UI:**
System tab → Storage & Game Folders → Game Scan Directories → Add Directory
opened the browser at the configured folder showing `PPSA02929-app0` and
`PPSA21564-app`; five presses of **Up** walked to "This PC" listing nine ready
drives with labels and "Select This Folder" correctly disabled.
Runs `SHELL_20260830_131248_ui02-picker`, `SHELL_20260830_131338_ui02-drives`.

## Defects found and fixed

### 1. Localization never loaded in the dev build (pre-existing)

`I18n.Load` looks for `assets/lang` next to the app and then walks up **five**
directory levels. The build output sits **six** levels below the repository root,
so it found nothing and `I18n.Tr` returned raw keys — the first fullscreen
screenshot rendered `view.library`, `button.play`, `library.search_hint` instead
of real text. `dist/pcsx5.exe` worked only by accident, because it happens to sit
inside the repository next to `assets/`; shipped anywhere else it would have had
the same failure.

Fixed in `Pcsx5Ui.csproj` by copying `assets/lang/*.json` into the output, so the
app is self-contained rather than depending on where it sits. Confirmed: the UI
now renders "Library", "Tools", "Play", "GAME LIBRARY".

This was not caused by this work — it was surfaced by it. It is exactly the class
of defect Rule 12 and the `ui-verify` skill exist to catch.

### 2. The verification harness sent input into the void

An early navigation run produced **byte-identical frames for its entire
duration**: every click was lost. `SendInput` delivers to whatever is focused,
not to a chosen window, and the shell was not foreground.

`screen_capture.focus_window()` now brings the target window forward using
`AttachThreadInput` (needed because `SetForegroundWindow` is refused for a
process that does not own the current foreground window), and `session.py` calls
it before dispatching any key or click. Without this, a UI verification run
reports "the UI did not respond" when the truth is that it was never asked.

`input_harness.py` also gained `click_at` / `double_click_at` / `move_to`, and
`session.py` a `--clicks "x,y@t;..."` schedule, because pointer input is the only
way to reach affordances that keyboard and gamepad navigation do not yet cover
(UI-03).

## Cross-cutting compliance (Rule 12)

Sixteen new locale keys were added to **all eleven** locale files, keeping key
parity intact. Non-English values are English placeholders: inventing
translations would be fabrication, and `check_ui_strings.py` reports them under
`identical_to_base` so a translator can find them.

Every new interactive control gets an `AutomationProperties.Name`, assigned from
code so the name is localized. That exposed a limitation in my own tool — a
XAML-only scan cannot see `AutomationProperties.SetName` — so the report now
counts code-set names as a separate metric rather than showing a misleading zero.

## Measurements

```bash
dotnet build src/ui_csharp/Pcsx5Ui.csproj -c Release -r win-x64   # 0 warnings, 0 errors
python tools/check_ui_strings.py --baseline tests/ui_baseline.json
ctest --test-dir build -C Release --output-on-failure              # 46/46
python tools/validate_ai_framework.py                              # PASSED
```

| Metric | Before | After |
|---|---|---|
| Windows folder dialogs in the shell | 3 | **0** |
| Hardcoded user-visible strings | 229 | **224** |
| `DynamicResource` refs (themeable) | 0 | **8** |
| Automation names set from code | 0 | **5** |
| Interactive controls | 119 | 122 |
| ...with a XAML `AutomationProperties.Name` | 0 | 0 |
| Locale keys, all 11 files | 98 | 114 |

`tests/ui_baseline.json` was updated deliberately to lock in the improvement, so
the ratchet now defends the better numbers.

## Verification questions

- **What changed?** Shell fullscreen startup; in-app folder browser replacing all
  Windows folder dialogs; locale packaging fix; input focusing in the harness.
- **What evidence justified it?** Measured counts before and after, plus driven
  UI runs with screenshots at each step.
- **What tests prove it?** The ratchet, the framework validator, CTest 46/46, and
  five `ui-verify` runs whose frames are cited above.
- **Did PPSA02929 regress?** Not applicable — no emulator code changed, and CTest
  is unchanged at 46/46.
- **Did screenshots confirm it?** Yes: fullscreen dimensions, translated text, the
  browser at a real path, and the drive list.
- **Speculation introduced?** None in behavior. The non-English locale values are
  explicitly placeholders, labelled as such.

## Follow-on rounds recorded here

UI-06 (typography), UI-09 (Tools argument) and UI-07 (input rebinding) were
completed after this document was first written; see the measurements table
above for the running totals and the task document for their acceptance
criteria.

## Remaining in this workstream

UI-03 (gamepad navigation completeness), UI-04 (unified PS4-style hint bar),
UI-05 (PS5 interface pass). UI-03 is next: 0 of 122 interactive controls carry a
XAML automation name, there is no keyboard navigation beyond F11, and initial
focus per screen is unset.
