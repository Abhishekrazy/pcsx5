# WALKTHROUGH-2026-08-30-ui-06-09-typography-input-library

Completion of UI-06, UI-07, UI-08 and the argument half of UI-09 from
`docs/tasks/TASK-2026-08-30-ui-console-experience.md`.

## Scope

`src/ui_csharp/**`, `assets/lang/*.json`, `Pcsx5Ui.csproj`. No emulator core
change; `tests/runtime_baseline.json` untouched.

## UI-06 — Typography and readability

Measured before: **177 of 252** `FontSize` attributes were 9, 10 or 11 px —
compact-desktop-tool sizes on a fullscreen console dashboard.

A named type ramp now lives in `App.xaml` (`TypeXS` 12 … `TypeDisplayL` 44) and
**265** literal sizes were replaced with `DynamicResource` references. The base of
the ramp rose (9→12, 10→13, 11→14) while display sizes, already legible, rose
only slightly. The entire interface is now retuned from one block.

### The clipping that followed

Larger text in fixed-size containers clipped glyphs and labels — reported by the
user and confirmed on screen. Containers were relaxed rather than the text shrunk
back: **54** button widths and **83** button heights became `MinWidth`/`MinHeight`,
plus **19** border heights. Controls now grow to fit content, which is also what
a localized UI needs — a German or Russian label runs 30–50% longer than English.

### A fullscreen fault the clipping check exposed

While verifying, the footer hint bar vanished behind the taskbar. Root cause: a
borderless window in the `Maximized` state covers the taskbar **only while it is
the foreground window**, and the verification harness had just taken focus.

`SizeToMonitor()` now sizes the window to the monitor's own bounds via
`MonitorFromWindow`/`GetMonitorInfoW`, converting physical pixels to DIPs through
`CompositionTarget.TransformFromDevice`. Coverage no longer depends on activation.
`Maximized` remains a fallback if monitor info cannot be resolved.

## UI-07 — Input screen and controller-driven rebinding

Rebinding did not work from the controller. Four interacting faults:

1. **Two timers competed for the same press.** `ControllerVizTimer_Tick` (60 Hz)
   captured the rebind while `ControllerTimer_Tick` (20 Hz) treated the identical
   press as navigation — Cross re-armed the binding, Circle cancelled it.
2. **No edge detection.** `CheckRebindInput` fired on a *held* button, so the
   Cross press that armed a binding was immediately consumed as its new value.
   Only the button used to arm could ever be bound.
3. **Circle was unbindable**, because navigation cancelled on it before capture.
4. **Capture was DualSense-only** — it sat behind
   `WindowsDualSenseReader.TryGetState`, so an XInput pad silently could not
   rebind at all.

`HandleRebindCapture` is now the single owner. It runs at the top of the input
tick, before every shortcut and navigation path, and consumes all pad input while
a binding is armed. It waits for the pad to return to neutral, then takes the
next press — so every face button including Circle is bindable. It reads the
normalized button mask the tick already builds for **both** device paths.
Cancel is the PS button (deliberately not bindable) or a 10 second timeout.

Capture was removed from the visualiser timer, with a comment recording why, so
the race cannot be reintroduced by someone restoring the "missing" call.

### Readable binding labels

The 26 binding buttons displayed raw internal tokens baked into XAML as
`Content` — `pad_up`, `axis_left_y`, `l1`. The default token moved into `Tag` as
`Slot|default_token` and the visible label renders from `I18n`:
`D-Pad Up`, `✕ Cross`, `Left Stick ↕`, `L3 (Left Stick Press)`,
`Touchpad / Share`. That also removed 26 hardcoded strings.

### Two defects introduced and caught during this change

Both were found from screenshots, not assumed away:

- `RefreshBindingLabels()` was called **before** `TranslateUi()`, so `I18n` had
  not loaded and every lookup fell back to the raw token. Moved after.
- The refresh walked `GetControllerSetupControls()`, which is the *navigation*
  list and omits six binding buttons (L3, Options, R3, Touchpad ×3). Those had
  their `Content` moved to `Tag` and were then never re-rendered — they appeared
  blank. A dedicated `GetBindingButtons()` now drives the refresh.

That second defect surfaced a pre-existing gap: those same six buttons were
**unreachable by controller** because they were absent from the navigation list.
They are now included.

## UI-08 — Library overhaul

The shelf read as a row of small thumbnails with a thin border for focus.

- Tiles **145 → 200 px**, corner radius 8 → 12, shelf row 265 → 340, shelf panel
  200 → 255, hero cover **220 → 300 px**.
- Focus is now unmistakable at distance: the focused tile scales to **1.14**, is
  fully opaque, carries a 34 px accent glow and is raised in Z-order, while
  unfocused tiles drop to **0.55** opacity. Colour alone never carried this.
- Hover lifts an unfocused tile to 0.85 so the pointer previews focus rather than
  speaking a different visual language.
- Tiles gained `AutomationProperties.Name` from the game title.

### A defect introduced and fixed in the same round

Enlarging the shelf turned the scroll arrows into large dark bars bleeding off
both edges. They are now 52×52, more translucent, and **hidden unless the shelf
actually overflows** (`UpdateLibraryScrollArrows`, deferred to
`DispatcherPriority.Loaded` because layout has not run when a tile is added).
With two games they correctly do not appear at all.

## UI-09 — Tools tab (argument half)

**The tab was not broken as first claimed.** It populated rows; that claim was
wrong and is corrected here. The real fault, visible in the user's own screenshot
as a `Usage: pcsx5_boot_parser [game_path]` banner, was that `RunBootAnalyzer`
passed `game.EbootPath` — a **file** — where the parser expects the game
**directory**. Given a file it prints usage and exits, so the analysis columns
(Format, Footprint) stayed empty while the grid looked alive.

Fixed, and verified: the log now reads `Parsing game at: …\PPSA02929-app0` with
full loader output.

Process handling was hardened at the same time: `Exited` could fire before the
output pipe drained (truncating results), and there was no timeout, so a stuck
parser would hang the tab indefinitely. Completion is now driven by the output
stream closing, with a 60 second bound and a kill.

`pcsx5_boot_parser.exe` is also now copied into the app output, because
`LocateBootParser()` probes a fixed list of relative paths that the dev tree
happens to satisfy and the packaged layout does not — the same class of defect as
the locale-path bug.

**Still outstanding:** `ParseParserOutput` does not extract Format/Footprint from
the richer output, so those columns still read "Unknown"/"-", and raw ANSI escape
codes (`[36m[Loader][Info]`) leak into the log pane.

## UI-09 completed — the analyzer now reports real data

Extraction was rewritten against the parser's **actual** output, captured by
running `pcsx5_boot_parser` directly against both titles. The previous code
matched markers the parser never emits, which is why the columns stayed empty
even after the argument fix.

Markers now used, all verified from real output:

| Column | Source |
|---|---|
| Format | `Detected SELF container` vs `Valid ELF64`, plus `treating as PIE` and `PS5 SDK module type 0x…` |
| Encryption | `SELF Seg[n]: … encrypted=N` counted across segments; plain ELF reports "Plaintext ELF" |
| Footprint | `Required memory footprint: … (size: N bytes)` |
| Alignment | every `Program Header n: type=0x1 … vaddr=0x… align=0x…`, flagged when `vaddr % align != 0` |

ANSI colour codes are stripped, so the log pane no longer shows raw
`[36m[Loader][Info]` escapes.

Results, both titles:

| | Format | Encryption | Footprint | Alignment |
|---|---|---|---|---|
| Dreaming Sarah | ELF64 · PIE (SDK 0xFE10) | Plaintext ELF | 7.58 MB | Misaligned (PH9 vaddr=0x4C6E50) |
| ASTRO BOT | SELF · PIE (SDK 0xFE10) | Decrypted (12 segments) | 250.89 MB | Misaligned (PH8 vaddr=0xEDF7E10) |

**The misalignment is a true finding, not a parsing bug.** Verified independently:
PPSA02929's PT_LOAD headers 0, 1, 2 and 4 are all 16 KB aligned; header 9 has
`vaddr=0x4C6E50` against `align=0x4000`, remainder `0x2E50`. The analyzer names
the offending segment so the result is actionable rather than a bare verdict.

Column widths were increased because the new values were being truncated.

## UI-04 — one hint bar, one render point

The PS4-style legend was assigned from 13 scattered sites, 11 of them hardcoded
English. All now call `ShowHints(key)`, the single render point, and the text
lives in the locale files. A screen that forgets its hints is now visible as a
missing call rather than as a silently empty bar.

## Packaged build staged to dist/

`dist/` was rebuilt so the controller rebinding can be tested on hardware, and
the **packaged layout was verified specifically** — that is where the locale-path
and parser-path defects would have bitten, since the dev tree satisfies the
relative-path probes and the packaged tree does not.

Staged: `pcsx5.exe` (single-file publish), `assets/lang/*.json` (11 files, 162
keys), `pcsx5_boot_parser.exe`, `pcsx5_core.dll`, `pcsx5_cli.exe`. The native
binaries were unchanged — no C++ was touched this round.

Verified from `dist/pcsx5.exe`: fullscreen start, translated text (not raw keys),
readable binding labels, and the analyzer producing the table above.
`session.py shell` gained an `--exe` override so a packaged build can be verified
rather than only the dev build.

## UI-03 — Navigation completeness

Three measured gaps, all closed:

**Focus was invisible.** There were **zero** `FocusVisualStyle` definitions in the
project. WPF's default focus adornment is a thin dotted rectangle that is
effectively invisible against this dark palette, so a controller user often could
not tell what was selected. An accent focus ring is now defined once in
`App.xaml` and applied to the four named control styles plus the eleven named
controls a pad actually lands on.

**No screen seeded focus.** None of the four tab handlers set focus, so switching
tabs left nothing selected at all. `FocusFirst(params Control[])` now seeds a
sensible target per screen, deferred to `DispatcherPriority.Loaded` because a
view that has just become visible has not been arranged yet and `Focus()` on an
unarranged element silently fails.

**Two surfaces were unreachable by pad:**

- `FullLibraryListView` — the "View All" list had *no* gamepad navigation
  whatsoever. Opening it stranded a controller user with no way to pick a game or
  get back. `HandleFullLibraryNav` adds D-Pad selection, Cross to choose, and
  Circle to return to the shelf.
- `SearchBox` — in no navigation path at all. Square now opens it from the
  Library screen; it was the only unassigned face button there.

Circle remains Back everywhere in navigation, per the user's confirmation that
this is the correct convention. The single exception is an armed rebind, where
every button must be capturable or Circle could never be assigned to anything;
PS is the cancel gesture in that state.

### A crash I introduced and fixed

The first attempt applied the focus ring through app-wide implicit styles using
`BasedOn="{StaticResource {x:Type X}}"`. **The shell failed to start at all** —
zero frames captured:

```
XamlParseException: Cannot find resource named 'System.Windows.Controls.ListView'
```

That `BasedOn` form does not resolve at App resource scope for every control
type, and an implicit style *without* `BasedOn` would replace each control's
default template outright, losing its appearance. The implicit styles were
removed and the ring applied explicitly instead. Caught immediately because the
run recorded `status: crashed` with `0xE0434352` and no frames — the harness
doing exactly its job.

## Controller input latency

The user reported the UI responding sluggishly to the pad. Two causes, both in
the polling path — the HID reader already runs on its own background thread and
hands back cached state, so it was not the bottleneck:

1. The input `DispatcherTimer` ran at **50 ms** *and* at the default
   **Background** priority. Background sits below rendering, layout and input
   processing, so those ticks are starved whenever the UI is busy and the real
   latency is far worse than 50 ms. Now 10 ms at `DispatcherPriority.Input`.
2. All directional input passed through a flat **220 ms** gate. That capped
   repeats at ~4.5/second and — worse — applied to a *change* of direction too,
   so pressing left straight after right waited out the cooldown before anything
   moved.

The gate is replaced with a normal key-repeat model: a new direction acts
immediately, a held direction waits 340 ms and then repeats every 90 ms.

Direction-change latency goes from up to 220 ms to effectively zero, and
worst-case poll latency from 50 ms to 10 ms. **Not measured against hardware** —
no pad is attached to this machine — so this is reasoned from the code, and the
user is best placed to confirm the feel.

## UI-10 — Hardcoded strings: 204 → 7

Rather than hand-migrating 204 literals, XAML now uses a localization markup
extension:

```xml
Text="{loc:Tr Key=library.header}"
```

`TrExtension` returns a **one-way binding** to an `I18nSource` indexer rather
than a string. That matters: a markup extension returning a string resolves once,
while XAML is parsed — which happens *before* the configured language is loaded,
so every label would render as its raw key. Binding means the text re-resolves
when `I18n.Load` raises `Refresh()`, which also makes changing language take
effect without a restart.

197 literals across both XAML files became `{loc:Tr}`, collapsing onto **178
distinct keys** (duplicates share a key, so a repeated label translates once).
English text moved into all eleven locale files, now **341 keys** each.

The **7** remaining are deliberate: brand and proper nouns that are identical in
every language (PCSX5, PS5, DualSense, Vulkan, WASAPI…) and runtime placeholder
readouts like "FPS: --" that code overwrites immediately.

Two defects caught during this, both from screenshots:

- The coverage tool did not recognise `{loc:Tr …}` as a markup extension, so it
  still reported 204. That was a gap in my own tool, not a failed migration.
- XML entities were carried into the locale values verbatim, so cards rendered
  "Display **&amp;** Video" — a bound string is not XAML-decoded. Ten keys were
  decoded. The first re-check still showed the entities because the locale files
  had been edited *after* the build and the output copy was stale; rebuilding
  confirmed the fix.

## Measurements

```bash
dotnet build src/ui_csharp/Pcsx5Ui.csproj -c Release -r win-x64   # 0 warnings
python tools/check_ui_strings.py --baseline tests/ui_baseline.json
ctest --test-dir build -C Release
python tools/validate_ai_framework.py
```

| Metric | Start of round | Now |
|---|---|---|
| Hardcoded user-visible strings | 224 | **204** |
| `DynamicResource` refs (themeable) | 8 | **273** |
| Automation names set from code | 5 | **6** |
| Locale keys, all 11 files | 114 | **142** |
| Font sizes below 12 px | 177 | **0** |
| Binding buttons unreachable by pad | 6 | **0** |

## An unresolved observation: one intermittent test failure

One `ctest` run reported **45/46** with a single failure. The failing test's
identity was not captured, because that invocation omitted `--output-on-failure`.
It did not reproduce in **five** subsequent runs, including one deliberately
started immediately after a shell session to provoke file contention.

Classification: `OBSERVED`, cause unknown. It is recorded rather than dismissed
because an intermittent failure that is explained away is how a real race gets
lost. Every future `ctest` invocation in this workstream will pass
`--output-on-failure` so the identity is captured if it recurs.

## Verification questions

- **What changed?** Type ramp and container relaxation; monitor-bounds fullscreen;
  unified controller rebind capture with readable labels; library tile sizing and
  focus treatment; boot parser argument, process handling and packaging.
- **What evidence justified it?** Measured counts before and after, plus driven
  UI runs with screenshots at every step; the parser argument fault was confirmed
  by running the parser directly with both a file and a directory path.
- **What tests prove it?** The ratchet, the framework validator, CTest 46/46
  (with the caveat above), and eight `ui-verify` runs.
- **Did PPSA02929 regress?** No emulator code changed.
- **Did screenshots confirm it?** Yes — every claim above about appearance is
  backed by a captured frame, and three of my own defects were caught that way.
- **Speculation introduced?** The rebinding capture path is **not** verified with
  a physical controller; no pad is attached to this machine. It is reviewed and
  builds clean, but a live press is unproven and is stated as such.

## Remaining in this workstream

UI-03 (navigation completeness), UI-04 (unified hint bar), UI-05 (further PS5
visual alignment), UI-10 (204 remaining hardcoded strings), and the two Tools
defects above. The open product question — whether the Tools tab should be built
out with the PKG/compat/crash tooling that exists headless, or reduced to one
working analyzer — is still with the user.
