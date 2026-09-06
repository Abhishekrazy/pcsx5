# PCSX5 Tasks

Progress tracker. Updated as soon as a task completes, and whenever new work is
discovered. Each task states what it is, why it matters, and what *done*
requires — a task whose justification lives only in a chat log is not trackable.

Status: `[ ]` open · `[~]` in progress · `[x]` done · `[!]` blocked · `[-]` dropped or falsified

Priority is `(silent-failure risk × blast radius)`, then whether it makes later
work cheaper. **Broad fixes before single-title work**: a defect in shared
machinery outranks one game's next step.

---

## Done

- [x] **Register the zero-fill tail of a PT_LOAD with the segment's own protection**
  `Memory::Protect` stamped a sub-range's protection onto the whole tracked
  region, so a module's writable `.bss` was reported read-only and every guarded
  write into it silently did nothing. Commit `68387e9`, test
  `TestPartialProtectKeepsRestWritable` (2 failures before, passes after).

- [x] **Report a memcpy that did not copy** — `MemcpyImpl` discarded
  `Memory::GuardedCopy`'s result, turning a refused write into a successful
  no-op. This is what hid the defect above. Commit `4abc0bf`.

- [x] **Sweep the whole unchecked-guarded-transfer class** — nine further
  guest-facing sites (`memset`, `memmove`, `realloc`, `strcpy`, `strncpy`,
  `strcat`, libc `strcat`/`strncat`/terminator) all discarded their result.
  Commit `9466de8`.

- [x] **Stop losing condition-variable signals delivered before the waiter parks**
  117 of 200 lost in a targeted test; 0 after. Commit `77e6f9a`, test
  `TestCondvarSignalRace`.

- [x] **Keep the buffer base address out of the draw-program cache key**
  Five distinct shader pairs re-translated 2,536 times because the key included
  a per-frame ring-allocator address. Now 5. Commit `c2880f1`, test
  `TestDrawLayoutHashIgnoresBufferBase`.

- [x] **Implement `feof`/`fgets`/`fgetc`** instead of returning a fixed
  end-of-file under nine NIDs. Commit `0a36f3e`. *(IMPLEMENTED, not VERIFIED —
  no title exercises them; see open task on a guest-filesystem fixture.)*

- [x] **Capture the emulator's own window surface** rather than the screen
  behind it. The harness was photographing overlapping windows and produced a
  wrong verdict on a real 20-minute run. Commit `86052a7`.

- [x] **Bound the surface capture and account for refused samples**
  `PrintWindow` had no timeout (measured blocking 23s and 38s), and refused
  captures were uncounted — which biases the classifier *toward* "progressing"
  exactly when data is missing. Commit `65a0270`.

- [x] **Make the draw-execution loss visible** — per-submit accounting of draws
  executed vs dropped. Measured live at 207 executed / 432 dropped.

---

## Plan

A roadmap survey (7 subsystem surveys, 3 competing roadmaps, 2 judge panels)
settled on this spine:

> The unit of planning is a defect **class**, not a title and not a lifecycle
> stage - because every expensive defect this project has paid for was one class:
> a refusal wearing the costume of a success. A class is cross-title and
> cross-subsystem by construction.

Order: make the instruments incapable of lying, close the APIs where discarding a
result is the easy path, give threads and TLS a real owner, make fabricated HLE
success and dropped GPU work loud, then ratchet each swept class shut.

---

## Discovered — the WPF shell (2026-09-03, from a user screenshot)

- [x] **Shell settings had no effect on the emulator at all** — FIXED
  The core defaults `config_dir` to the *relative* path `"pcsx5_config"`,
  resolved against the child process's working directory. `IpcSession` pins that
  to the folder holding `pcsx5_cli.exe` — it has to, because the Windows loader
  must find `pcsx5_core.dll` before `main()` runs. The shell writes its config
  next to its own executable. So the shell saved to
  its own `pcsx5_config/global.json`, beside the shell executable, while the core read
  `build/bin/Release/pcsx5_config/global.json`.
  Measured: the shell's copy had `audio.backend: 1`, the core's had `0`. The
  user had turned sound on; the file saying so was never opened.
  Fixed by passing `--config-dir=<absolute>` to the child, a flag the core
  already supported. Verified end to end: with the shell's config dir the core
  logs `sceAudioOut: WASAPI mix is 44100 Hz` and opens a port; with the old
  default it never attempted audio.

- [ ] **WASAPI init fails on a 44.1 kHz output device and silently drops to
  waveOut.** Surfaced by the verification above:
  `WASAPI mix is 44100 Hz/2 ch (port wants 48000 Hz stereo)` →
  `WASAPI init failed; falling back to waveOut`. Sound works, but the shared-mode
  path a user selected is not what they get, and the reason is a resample the
  backend declines to do. Done means: resample to the device mix rate, or report
  the downgrade where a user can see it rather than only in the log.


- [ ] **The boot overlay is stuck at "Step 1 of 6 / 15%" while the console shows
  module linking.** Root cause found, two parts, both in `GameSession.cs`:
  1. There are **two parallel launch implementations**. `GameThreadProc` runs the
     core in-process through `pcsx5_init`/`pcsx5_load`/`pcsx5_run` and raises
     honest phases from the actual call it is inside. The shell does not use it:
     `Launch` (line 162) calls `_ipc.Launch(...)`, which spawns `pcsx5_cli.exe`
     as a **child process** over a shared-memory/pipe IPC. Two implementations of
     the same responsibility, and the dead one is the one with real progress.
  2. On the IPC path the phases come from "phase sniffing from log keywords"
     (`OnCoreLog`, ~line 358): `text.Contains("link") || text.Contains("reloc")
     || text.Contains("module")`. That is the bare-substring marker pattern
     `tools/game_runner/boot_markers.py` already documents as discredited and
     retired — it matched `menutitle-sheet0.png` for "menu". Worse, it is gated
     on `State == GameSessionState.Booting`, and `Launch` sets
     `State = Running` as soon as the child process starts, so **the sniffing
     never runs at all** and the overlay stays on its initial values.
  Done means: one launch path, and boot progress reported by the core over the
  IPC channel it already owns, rather than guessed from log substrings by the UI.

- [ ] **No menu music in the shell: audio defaults to Off and three config trees
  disagree.** `src/config/config.h:72` — `int backend = 0; // 0=Off, 1=WASAPI`.
  `pcsx5_config/global.json` (repo root) has `"backend": 0`;
  `dist/pcsx5_config/global.json` has `"backend": 1`; `.work/dbg_config/` is a
  third. `IpcSession` sets `WorkingDirectory` to the folder holding
  `pcsx5_cli.exe`, so which config the shell gets — and therefore whether there
  is any sound — depends on where the binary happens to live.
  `CoreBridge.Pcsx5Options` has no audio field at all, so the shell cannot
  override it even deliberately.
  Done means: the shell can set audio explicitly, and the config trees stop
  silently disagreeing about a user-visible default.

- [ ] **The shell passes `--headless` to the core it wants frames from.**
  `IpcSession.cs:122`. Flagged, not yet explained — it may be correct for the
  IPC frame-sharing path, but a shell that renders frames asking for headless
  needs a reason recorded next to it.

- [x] **The Gemini rulebook was deleted deliberately; dangling citations removed**
  `CLAUDE.md` and the `.claude/` rules, skills and agents cited 33 companion rule
  files and `GEMINI.md` as binding. The repository owner confirmed on 2026-09-05
  that they were deleted on purpose and are not wanted back, so the citations
  were removed rather than left pointing at missing documents — a contract that
  cites a document nobody has cannot be followed and cannot be checked.
  Anything stated only in that rulebook is no longer in force; the thirteen rules
  in `.claude/rules/` are now the whole of it.
  This was a real `doc_links` failure doing its job: it had been red locally
  since the deletion. `check_doc_links` now reports 202 referenced paths, all
  resolving. It passed on CI throughout, because `.claude/` is absent there too
  and neither set is scanned — worth remembering the next time CI green is
  mistaken for a clean tree.

## Phase 4 - Rebuild the shell's Input tab (approved 2026-09-06)

The user asked for the Input tab rebuilt from scratch, with the controller
read through the **native core** rather than the duplicate C# reader (decided
2026-09-06, resolving ADR-001 steps 2-3). Requirements, verbatim in intent:

**Device information:** battery level with power state; firmware versions
(main, SBL, DSP) and model revision with manual refresh; connection status --
headphones/mic jacked in, microphone muted, USB data and power.
**Real-time monitoring:** every button, stick, trigger, motion sensor with
gyro/accelerometer graphs, and touchpad; USB/Bluetooth detection; multiple
controllers with an in-window picker.
**Tests:** input test that **locks tab switching** while running; a speaker
test; a haptics test.
**Art:** the Gamepad-Asset-Pack (MIT, attribution required) with credit given.

What the core already has: buttons, sticks, triggers, touch, accel, gyro,
battery level/charging/full, headphone-connected, `IsBluetooth()`, verified
speaker and haptics playback. What it does not: firmware info (feature report
`0x20`), mic-mute state, USB data/power, and anything beyond controller 0.
What the shell has: **no pad exports in `CoreBridge` at all**.

Ordered by dependency, one subsystem per change (Rule 10):

- [x] **4.1 Pad-state ABI in `CoreBridge`** - DONE. Six additive cdecl exports
  (`pcsx5_pad_count`, `_get_state`, `_get_firmware`, `_set_audio_levels`,
  `_play_speaker_test`, `_play_haptics_test`), all confirmed present in the
  built DLL. Two POD structs with a `struct_size` guard so a C#/C layout drift
  fails loudly instead of reading garbage. Nothing existing changed. 52/52
  ctest, 0 warnings, shell builds against the mirror.
- [x] **4.2 Firmware info in the core** - DONE and **VERIFIED on hardware**.
  Feature report `0x20` read on the user's pad:
  `main 1.16.42  sbl 0.1.42  dsp 0002_000A  model rev 0x0414  gen 4  built Jul 4 2025 10:10:32`.
  The build date is the proof: an ASCII date and time parsing cleanly at the
  inferred offsets [1..11] and [12..19] cannot be coincidence, so the whole
  offset table is promoted from INFERRED to VERIFIED.
- [~] **4.3 Mic-mute and USB data/power state in the core** - IMPLEMENTED,
  still INFERRED. Read from the input report's status byte at evaluator
  offset 0x35 (`hidBuffer[2+0x35]` on Bluetooth, `[1+0x35]` on USB): 0x01
  headphone, 0x02 mic jack, 0x04 mic muted, 0x08 USB data, 0x10 USB power.
  On Bluetooth with nothing plugged in every bit read 0, which is *consistent*
  with the assignments but does not prove them. Promotion to VERIFIED needs
  the user to press mute and plug a cable while the probe runs and confirm the
  right bits flip. Note the 0x08 bit is the one DualSenseWindows labels
  "charging"; the two readings agree in practice and disagree in name.
- [~] **4.4 Multiple controllers in the core** - IMPLEMENTED; two-pad test
  pending. The reader now holds up to 8 `PadSlot`s keyed by HID device path,
  so a pad keeps its index while connected and unplugging pad 1 does not
  renumber pad 2. One thread round-robins the bound slots; enumeration runs
  only while a slot is free and at most twice a second. Every index-less API
  function forwards to pad 0, so the twelve external callers compile unchanged.
  Verified: 0 errors, 0 warnings across the 20 targets that compile the
  reader; 52/52 ctest; probe shows `1 of 8 slots`, pad 0 live, firmware and
  status identical to before the rewrite. **Not yet verified:** two physical
  pads, and that removing one leaves the other's index intact. Needs the
  user's second DualSense paired.
- [x] **4.5 The Input tab itself** - DONE and SEEN. InputTabView (XAML plus
  code-behind, a focused class per Rule 11) reads only through CoreBridge.
  Picker from every connected slot, transport, device information, firmware
  with refresh, motion graphs, the DualSense drawn from the vendored art via
  the VSCView layout evaluated as data. 57 I18n keys in all eleven locales,
  every control named for automation, brushes via DynamicResource, ratchet
  unchanged. Screenshot: artifacts/runtime/SHELL_20260906_020604/frames/frame_0024.png
  (commit ca0609e; its message carries an unfilled {RUN} placeholder - this
  is the path it meant). The old image-with-overlays block and live-tester
  panel are gone with 25 methods, 6 fields and 2 buttons; the mapping editor
  is untouched.
- [x] **4.6 Tab lock during tests** - DONE. InputTabView.IsTestRunning is
  checked by all four tab handlers, which the gamepad L1/R1 navigation also
  routes through, so one guard covers pad, mouse and keyboard. The refusal is
  logged and shown in the footer with input.tabs_locked.
- [x] **4.7 Speaker and haptics test buttons** - DONE. Three tests through
  the core built-in exports on a worker thread; over USB the audio tests say
  the lane does not apply and stop, rather than driving a Bluetooth report at a
  device that is not listening. Visible in the same screenshot as 4.5.
- [x] **4.8 Controller art vendored** - DONE, ahead of 4.5 because the tab
  needs the files. `assets/gamepad/dualsense/`: 29 sprites and `layout.json`
  from VSCView (MIT), the white body templates from Gamepad-Asset-Pack (MIT),
  both licences verbatim, and a README with credit text and the pinned commits.
  The two projects ship the same art (26 of 29 sprites byte-identical), and
  VSCView's theme is its canonical placement, so sprites and layout come from
  one source and cannot drift. The layout JSON is consumed directly, not
  transcribed. The ripped-vs-recreated caution is recorded in the README: the
  pack does not say per file, so the honest position is that it is unknown.
- [~] **The About text is stale and there is no credits surface.** (Credits
  surface DONE 2026-09-06: System Information > Credits opens the README's
  Credits section. The stale `about.line3/4` strings remain.)
  `about.line3` reads "UI: Dear ImGui + GLFW + OpenGL3" and `about.line4`
  "Layout: top toolbar + grid + bottom console" - the ImGui shell that no
  longer exists - and nothing in the WPF shell references `about.*` or shows
  any third-party credit. Found while looking for somewhere to put the
  attribution the vendored controller art requires. Done means: correct the
  four lines, and add a credits line (VSCView, Gamepad-Asset-Pack,
  DualSenseWindows, LibAtrac9, libopus, NAudio, Squirrel) shown under the
  System Information hub, in all eleven locales.

- [x] **D-pad decoded wrongly since the reader swap: Up asserted at rest,
  every direction mis-mapped.** FIXED and SEEN. DualSenseWindows already converts the hat into
  a bitmask (LEFT 0x01, DOWN 0x02, RIGHT 0x04, UP 0x08) in the low nibble of
  `buttonsAndDpad`; `MapButtons` still read that nibble as the raw 0..7/8 hat
  value the previous in-header reader exposed. So centred (0) became Up, a
  real Up (0x08) became nothing, Left became up-right. Observed twice
  independently: the hardware probe reported `buttons 0x00000010` before
  anything was touched, and the rebuilt Input tab lit the Up sprite on an idle
  pad. Games received D-pad Up held forever. Fixed in `MapButtons` with four
  bit tests; awaiting the native rebuild, the probe showing `0x00000000` at
  rest, and 52/52. No unit test covers `MapButtons` (anonymous namespace) -
  recorded as a gap rather than papered over.
  Verified: 52/52 ctest, 0 warnings, and the Input tab screenshot at
  `artifacts/runtime/SHELL_20260906_015955/frames/frame_0025.png` shows the
  Up sprite unlit on an idle pad where the previous capture showed it lit.

- [x] **RESOLVED 2026-09-06 (see "Core: DualSense accelerometer misparsed" below - it was the accel/gyro field swap). Was: NEEDS_EVIDENCE: the accelerometer shows no gravity axis at rest.**
  With the pad lying still, the Input tab autoscaled accelerometer trace is
  jitter around zero on all three axes and no constant line; a gravity-sensing
  accelerometer must show roughly 1 g on one axis. Either the DualSenseWindows
  accelerometer/gyroscope field mapping, or the report offsets it applies over
  Bluetooth, deserves a check against a known orientation. Recorded from
  artifacts/runtime/SHELL_20260906_020604/frames/frame_0024.png; not changed until measured.

- [ ] **Sensor scale is UNKNOWN: `Sample.accel/gyro` are raw counts, not g
  or rad/s.** The header said "in g (approx)"; the reader stores
  DualSenseWindows' raw integers unchanged. Found when the Input tab graphed
  them as g and every near-zero axis clamped into a full-height square wave.
  The comment is corrected and the graphs now autoscale rather than assume a
  unit. Establishing the count-per-g and count-per-(deg/s) needs a measurement
  - a pad held still on each axis, and a known rotation - not a value copied
  from a reference.

- [x] **Harness: --keys accepted unknown key names and sent nothing.** FIXED.
  VK_CODE had no function keys, and send_key_press treated an unknown name as
  a silent no-op, logging it as pressed. Found when f11 was scheduled to put
  the shell fullscreen for a capture and the window stayed 1200x780 with no
  error. F1-F12 and Tab added; an unknown name is now a SystemExit listing the
  known keys. Verified by the same schedule then producing a full-width frame.

- [ ] **4.10 Full controller support for shell navigation** (asked 2026-09-06:
  "I want full support of the Controller for UI navigation"). Today the shell
  has partial pad navigation - L1/R1 switch tabs, D-pad and Cross/Circle work
  on the Library grid and the Controller-setup control list - driven from
  `ControllerTimer_Tick` through the C# reader. "Full" means every screen and
  every interactive control reachable and operable by pad: Settings hubs and
  their rows, the Input tab's picker and test buttons, dialogs, the console
  dock, the folder picker, first-run setup. Done means: a focus model that
  every view registers with rather than a per-screen control list, visible
  focus, PS4-style on-screen hints on every screen, and the pad read through
  the core (which makes this the same change as 4.9, not a separate one).
  Verify by driving every screen with the pad alone and screenshotting each.

- [~] **4.13 Shell redesign: "Void console" with themes** - asked 2026-09-06
  after approving the concept canvas (artifact 12445a87): implement it in
  the shell, add a light/dark theme mode, and make the theme colours user
  controllable in Settings. Spec: docs/tasks/TASK-2026-09-06-shell-redesign-void-console.md;
  decision: architecture/decisions/ADR-003-shell-theming.md. One screen per
  change, each seen in dark and light with a non-default accent.
  - [x] **Step 1 - theme foundation** - DONE. Twelve token brushes in
    App.xaml; `Theme.cs` (two palettes, "system" from Windows' apps-theme
    key, user accent with on-accent text picked by luminance, optional
    ground); config `ui.theme/accent/ground` persisted in config.ini;
    Settings > UI & Personalization gains Theme / Accent colour /
    Background colour as pad-navigable choices (9 accent swatches, 7
    grounds) localized in all eleven locales. Seen: the three entries with a
    coral accent read from config.ini -
    artifacts/runtime/SHELL_20260906_034218/frames/frame_0010.png. What is
    NOT yet visible: light mode and the accent on existing screens, because
    every existing screen paints literals; only the AccentFocusVisual ring
    follows the accent today. That is by design (ADR-003) and lifts screen by
    screen below. Colour-name labels for the ground presets are hardcoded
    English in C# (proper-noun-like); to localize if the ratchet ever
    counts C#.
  - [x] **Step 2 - Library (main) on tokens** - DONE. Window ground and
    text, title bar and tabs, the hero (CONTINUE eyebrow, title, mono pills,
    accent Play with on-accent text and a focus glow), the cover card, the
    RECENT shelf header, scroll arrows, the secondary button style and the
    footer legend all reference theme tokens; the tab highlight, tile
    selection ring and compat badge colours come from the tokens in
    code-behind. Every x:Name the code-behind uses is unchanged. The
    bottom-anchored hero had grown past its row and clipped its first child
    (the eyebrow) - fixed by shrinking the shelf row to 400 px. Seen: dark
    with cyan - artifacts/runtime/SHELL_20260906_034913/frames/frame_0005.png;
    light with coral - SHELL_20260906_034731/frames/frame_0005.png (same
    layout, before the row fix). DynamicResource Theme* references in
    MainWindow.xaml: 34 (was 0). Still literal on this screen: the search box,
    the tile placeholder brush and the blurred backdrop overlay.
  - [x] **Step 3 - All games on tokens** - DONE. Header per the concept:
    Back, "All games · N", a search field that filters by title or ID as you
    type, and a five-way segmented sort (Recently played / Title A-Z /
    Title ID / Size / Favourites) with the active segment painted from the
    tokens. Favourites live in a focused `Favourites.cs` (favourites.json
    beside config.ini); a starred tile shows an accent star. Pad: Square
    toggles favourite, Triangle cycles the sort, Options focuses search; the
    legend gained an Options glyph (glyph.opt.*) in all locales. The
    ComboBox sort picker is gone. Seen: favourite toggled and sorted A-Z by
    keyboard in light/coral -
    artifacts/runtime/SHELL_20260906_041244/frames/frame_0011.png; dark with
    the corrected legend - SHELL_20260906_041341/frames/frame_0008.png.
  - [x] **Keyboard died with the controller** - found and fixed on the way
    (belongs to 4.12): ControllerTimer_Tick returned at "no controller
    connected" before folding the keyboard in, so once the DualSense
    powered off, R1 and every other key did nothing (SHELL_20260906_041139:
    R1 sent, no tab change, footer stuck on its XAML default). It now
    carries on with an empty pad state. Verified: Square and Triangle drove
    the All-games screen by keyboard with no pad attached
    (SHELL_20260906_041244, favourites.json written).
  - [x] **Corner clipping fixed; corner style is a setting** - DONE. Asked
    2026-09-06: "rounded corner issue almost everywhere ... corner setting
    also should show in settings, like rounded, hard edge and corner cut".
    The defect: a Border's CornerRadius never clips its children, so cover
    images poked square corners out of rounded cards (hero cover, shelf and
    grid tiles). Fix: `Theme.ClipCorners` attached property clips an element
    to the current corner style and follows its size and theme changes; set
    on the hero cover, the All-games tile and the shelf tiles. Style: five
    CornerRadius tokens (ThemeCornerS/M/L/XL/Pill) replaced all 62 literal
    CornerRadius values in MainWindow.xaml and 1 in App.xaml, rewritten by
    Theme.Apply for rounded / sharp / cut; `ui.corners` persisted in
    config.ini and offered in Settings > UI & Personalization (localized in
    all eleven locales). Seen, All games in each style: rounded
    artifacts/runtime/SHELL_20260906_041914/frames/frame_0006.png; cut
    SHELL_20260906_041927/frames/frame_0006.png; sharp
    SHELL_20260906_041939/frames/frame_0006.png. Pills become rectangles
    under sharp and cut by design.
  - [x] **Step 4 - Settings on tokens** - DONE. All three layers (hub of
    category cards, the per-category list, the option sub-page) reference
    the tokens: the four Ps5* styles (tile, row, option, pill toggle) with
    their hover and focus states, every literal in the SettingsView block and
    the two sub-page views (25 XAML literals), and 17 literal brushes in the
    code-behind row and option builders. The seven rainbow category-icon
    tints became one soft accent tint, per the concept. Value badges use
    the text token over the soft accent so they stay readable in light.
    Navigation is unchanged (hub -> list -> choices); the concept's
    side-section layout is a later step if wanted. Seen: dark hub and list
    artifacts/runtime/SHELL_20260906_042407/frames/frame_0004.png and
    frame_0010.png; light with coral, list and Theme choice page
    SHELL_20260906_042601/frames/frame_0008.png and frame_0011.png.
  - [x] **Hero cover shown whole; strokes survive the clip** - DONE. Asked
    2026-09-06: "fix border issue and main menu big image of title not
    showing fully". The hero cover was painted UniformToFill, so any
    non-square cover was cropped; it now paints Uniform into an inner
    element of a fixed 300x300 card, whole, on the surface colour. The
    border issue: the corner clip was applied to the bordered element, so
    its own stroke lost half its width at every corner. Rounded now clips
    the Border's child (stroke intact, inner radius reduced by the stroke);
    cut clips the element so the stroke follows the chamfer; sharp clips
    nothing. Seen: cut artifacts/runtime/SHELL_20260906_043015/frames/frame_0005.png,
    rounded SHELL_20260906_043026/frames/frame_0005.png.
  - [x] **Step 5 - Input on tokens** - DONE. InputTabView.xaml referenced
    the old fixed brushes (10 ForegroundMutedBrush, 4 GlassBackgroundBrush),
    now ThemeTextMuted / ThemeSurface; its code-behind's touch-point dots
    follow the accent and the graph midlines the hairline (the three axis
    trace colours stay fixed as chart series). The mapping editor below it
    in MainWindow.xaml had 79 literals (white text, #0099FF accents,
    hairlines, raised fills, the combo boxes) - all on tokens now. Seen:
    dark artifacts/runtime/SHELL_20260906_043210/frames/frame_0006.png; light
    with coral SHELL_20260906_043222/frames/frame_0006.png (pad was off, so
    the device panel shows dashes; the mapping editor is below the fold at
    the harness height and was verified by the literal count, not seen).
  - [x] **Step 6 - Tools on tokens** - DONE. The Boot Analyzer view's
    literals (muted text, hairlines, surface, the list's item-container
    style with its selection and hover fills, the log box) are on tokens; the
    log box uses the muted-text token rather than warning yellow, matching
    the concept's console. Seen: dark
    artifacts/runtime/SHELL_20260906_043547/frames/frame_0006.png; light with
    coral SHELL_20260906_044218/frames/frame_0006.png.
    FALSIFIED on the way: "the log box ignores a DynamicResource foreground"
    - two tokens and a code-assigned brush all looked the same yellow to me
    at 10.5 px, while a literal red clearly painted. A runtime dump showed
    the box, its resource and its inner TextBoxView all at #5B6373, and a
    pixel sample of the frame put the log text at exactly #5B6373, the same
    as the panel title. The "yellow" was ClearType fringing on small glyphs
    read by eye. Lesson recorded: judge colour by sampling pixels, not by
    looking at a downscaled frame.
  - [x] **Step 7 - Console on tokens** - DONE. The console panel's ten XAML
    literals (surface, hairlines, muted labels, the ground behind the host
    presenter, an accent-tinted border) are on tokens, and the six fixed
    per-level line colours (green info, yellow warn, ...) now come from the
    tokens - muted for trace/debug, text for info, warning and danger for
    the rest - so lines read on a white surface too. Lines already written
    keep their colour on a theme change; new ones follow. Seen: dark
    artifacts/runtime/SHELL_20260906_044710/frames/frame_0006.png; light with
    coral SHELL_20260906_044722/frames/frame_0006.png.
  - [x] **Step 8 - Error overlay on tokens** - DONE. The crash diagnostics
    overlay's 25 literals (scrim, card, raised panels, text, muted, accent,
    danger, success, borders) are on tokens; the shared SecondaryButtonStyle
    (Copy / Raw Logs / Dismiss here, used elsewhere too) was still literal
    and is on tokens now. The three DropShadowEffect glow colours stay
    literal (an effect's Color cannot take a brush token; small, decorative).
    Seen by launching Super Monkey Ball, which crashes on boot: dark
    artifacts/runtime/SHELL_20260906_045056/frames/frame_0024.png; light with
    coral SHELL_20260906_045256/frames/frame_0024.png.
  - [x] **Step 9 - Pause overlay on tokens** - DONE by literal count, NOT
    seen: 20 literals (card, text, muted, accent and its soft fills, danger
    for Stop) are on tokens; the scrim stays a translucent black on purpose,
    since it must dim a live game frame in either theme. It could not be
    seen running because no game reaches gameplay under the shell right now
    (next item). Verify visually once that is fixed.
  - [ ] Step 10 - Quick settings popup: SOFT BOUNDARY (2026-09-06). The
    IPC pipe carries only STOP / KILL / PAUSE / RESUME (ipc_shared.h), so a
    mid-game settings popup could apply nothing to the out-of-process core;
    building it now would be a stub. Needs an IPC "set option" command with
    the core applying runtime-safe settings (volume, frame limit, VSync,
    resolution scale) and a reply; own iteration, after the embedded-input
    ownership decision under 4.14.
  - [x] **Step 11 - Folder picker on tokens** - DONE. The picker's own
    Picker* brushes were fixed dark values in App.xaml; Theme.Apply now
    derives them from the palette (surface, ground, hairline, text, muted),
    so the picker follows dark/light and the accent. Two defects found and
    fixed on the way: its legend was shown raw ("%DEV% %DIR% Move ...")
    because the text bypassed the glyph substitution the footer uses; and
    its selected row was the Windows system highlight, now a container style
    on the soft accent fill. Seen: dark
    artifacts/runtime/SHELL_20260906_061034/frames/frame_0012.png; light
    with coral (before the two fixes) SHELL_20260906_060850/frames/frame_0012.png.
  - [x] **Step 12 - Booting screen** - DONE. Per the added artboard: STARTING
    eyebrow, title, a mono "title id · step n of 6" line, a 3 px accent
    progress line, six staged rows (Core, Package, Executable, Modules, Guest
    CPU, First frame) with done and current stages lit, the stage name and
    detail, Cancel Boot, and the cover in a clipped card; no spinner. All
    tokens; stage labels localized in eleven locales. The screen owns the
    game area until the first guest frame: the whole embed (host creation
    included) is deferred while it is up, because a native host window
    paints over WPF even with its child hidden (found when the first cut
    showed an empty dark host instead of the screen). On the Running phase
    the screen hides, the core window is embedded and shown, and the footer
    reads Running. Seen: artifacts/runtime/SHELL_20260906_061736/frames/frame_0013.png
    (stages at step 5) and frame_0016.png (game revealed, "Running").
    Resolves the 4.14 note about the core's text-mode boot screen showing
    through. The stage titles in UpdateBootPhaseUI remain hardcoded English
    in C# (pre-existing; the ratchet counts XAML).
  - [~] **Step 13 - Stuck notice** - detection DONE and VERIFIED; rendering
    over the embedded game NOT yet visually confirmed. The core ticks the IPC
    frame counter on every guest flip (IPC::TickFrame, exported the same way
    as the frame/state sinks) even though pixels no longer travel over IPC;
    the shell's frame poller raises FrameStalled(seconds) only after the game
    has drawn at least once and >= 6 s without a new frame, and FrameResumed
    on the next frame. The notice is the concept's card (warning dot, "No new
    frames for N s", body, Wait / Force Stop), on theme tokens, localized in
    eleven locales; a normal boot never triggers it and a paused game is
    exempt. VERIFIED by suspending the core process mid-run (a real stall):
    shell console shows "No new frames for 6 s" then "Frames resumed" and the
    frame counter recovers (SHELL_20260906_063100). NOT DONE: showing the
    card ON TOP of the embedded game. The emulator is a native child window
    and paints over every WPF element in the same window (airspace), so the
    notice is hosted in a separate top-level OverlayWindow tracked to the
    game area's bottom-right; it is created, positioned (logged at
    1096,682 440x160) and reports visible, but an independent full-screen
    grab mid-stall did not show it, so its rendering above the native child
    is unconfirmed. Own focused iteration: WPF airspace over the embedded
    HWND (a DirectComposition/HwndHost sibling, or drawing the notice inside
    the core's window). The notice renders correctly in the main window when
    no game is embedded (the pre-embed watchdog path). Steps 6-11 - Tools, Console,
    Error, Pause overlay, Quick settings, Folder picker.
  - **Layout rebuild pass** (asked 2026-09-06: "rebuild each screen layout to
    match the artboard start with main screen, footer, title, chips etc").
    Steps 1-13 put screens on tokens without changing their shape; this pass
    rebuilds each screen's structure to the concept artboard (artifact
    12445a87). One screen per change, each seen running.
    - [x] **Library (main)** - DONE (commits f416826 back). Flat ground, hero
      eyebrow/title/mono chips/Play+Settings/cover, RECENT shelf, and a footer
      control legend of glyph-box chips.
    - [x] **Folder picker** - DONE 2026-09-06. Rebuilt the overlay to the
      FolderPicker artboard: a centred card on flat theme tokens (no accent
      glow), header "Add a games folder" with the eboot note, a mono
      breadcrumb of the current path (last segment bright), quick drive chips
      with the current drive first and highlighted, an eboot-aware directory
      list (folders holding an eboot.bin get the accent game icon and an
      "eboot.bin · SIZE" mono meta, sizes in the concept's short form via
      FormatSizeShort), a Cancel / "Add this folder" pair bottom-right, and
      the controller glyph legend along the bottom (BuildFooterHintChips
      retargeted to the picker footer). The old Up/Open buttons are gone
      (the ".." row, double-click and the pad do that); their two
      AutomationProperties.SetName sites were replaced by names on the drive
      chips and the breadcrumb so accessibility coverage did not regress. Two
      new keys (picker.subtitle, picker.up_one) in all eleven locales;
      picker.title/select reworded to the artboard in en-US. Built clean
      (0 warnings), ui-strings ratchet unchanged. Seen (via a temporary
      env-gated auto-open, since removed):
      artifacts/runtime/SHELL_20260906_112825/frames/frame_0000.png.
    - [x] **Tools** - DONE 2026-09-06. The Tools tab used to land straight on
      the Boot Analyzer table; per the Tools artboard it now lands on a hub of
      tool cards (ToolsHubView): a "Tools" title and a 3-column grid of cards
      (title + description, 170 px, theme tokens, accent focus ring). Boot
      analyzer is the one live card and opens the analyzer as a sub-view (an
      added Back button returns to the hub); the other five artboard tools
      (Crash reports, Extract package, Shader cache, Input recorder, Memory
      map) are shown disabled with a "Soon" tag rather than faked, honest
      about what exists. Tab switching, ShowTab, the pad L1/R1 tab cycling
      (hub counts as the Tools tab) and per-tab pad nav (Cross opens Boot
      analyzer, Circle back to Library; footer hints.tools) all account for
      the hub. Fifteen new keys (six card title/desc pairs, tools.soon,
      tools.back, hints.tools) in all eleven locales. Built clean (0
      warnings), ratchet unchanged. Seen (temporary env-gated auto-open, since
      removed): artifacts/runtime/SHELL_20260906_121158/frames/frame_0000.png.
    - [x] **Boot analyzer detail (inside Tools)** - DESIGN DRAFTED then BUILT,
      DONE 2026-09-06. Design: a BootAnalyzer artboard on the concept canvas
      (artifact 12445a87), approved by the user. Implementation: AnalyzerView
      rebuilt from the old GridView table + raw log to the artboard - a
      breadcrumb + Analyze all / Export summary header, a titles table whose
      rows carry a per-title result chip (ok/warn/fail coloured from the
      ThemeSuccess/Warning/Danger tokens via AnalyzerStateBrushConverter), an
      accent selection ring, and a right-hand report bound to the selection:
      the title's facts (format, encryption, footprint, alignment), a
      boot-readiness checklist, and the parser log. IMPORTANT divergence from
      the mockup, on purpose (Rule 04): the checklist shows only stages the
      STATIC parser actually evidences - container recognised, segments
      mapped, decrypted, alignment, module loaded - NOT the mockup's runtime
      stages (relocations / imports / first frame), which the static parser
      cannot know. Result verdict and stages are derived in ParseParserOutput
      from the real parser signals (BootStage list on BootAnalysisResult).
      Seventeen new keys (subtitle, result, readiness, parser_log, six result
      verdicts, five stage labels, loaded/not_loaded) in all eleven locales.
      Follow-up 2026-09-06 (user: "let user select and analyze one/multiple/all,
      add a search bar"): the tab no longer auto-analyzes on open. It lists the
      games as unanalyzed rows (BootAnalysisResult is now INotifyPropertyChanged
      so rows fill in place); each row has a checkbox, the header a select-all,
      and a search box filters by title or ID via the collection view. "Analyze
      selected" runs the parser only for the checked rows (or the focused row if
      none are checked) and updates each live; the Library "Analyze" action
      checks and analyzes just that one title. Six more keys (search, select_all,
      analyze, pick_first, running, not_analyzed) in all eleven locales. Built
      clean (0 warnings), ratchet unchanged. Seen idle
      (artifacts/runtime/SHELL_20260906_144801/frames/frame_0000.png) and after
      select-all + analyze (SHELL_20260906_144825/frames/frame_0000.png).
    - [~] **Input** - SLICE 1 DONE 2026-09-06. The Input tab is the largest,
      most functional screen (live DualSense visualizer + device panel + tests
      + a working rebind mapping editor, ~900 lines across InputTabView and
      ControllerView), so it is staged. Slice 1 rebuilt the live panel
      (InputTabView.xaml) to the concept artboard: hairline-border cards on
      flat tokens (ThemeCornerL), the pad diagram in a raised card, a transport
      pill by the picker, device rows as label/value DockPanels, motion graphs,
      and a new "Keyboard & mouse" card; test/refresh buttons use a local
      OutlineButton style (the shell's SecondaryButtonStyle lives in
      MainWindow's own resources and a separate UserControl cannot see it via
      StaticResource - that mismatch crashed the first attempt at load,
      artifacts/runtime/SHELL_20260906_145612, fixed by the local style). The
      ControllerView header is now a display-size title. All live x:Names and
      handlers preserved. Two new keys (input.kbm, input.kbm_help) in eleven
      locales. Built clean (0 warnings), ratchet unchanged. Seen:
      artifacts/runtime/SHELL_20260906_145716/frames/frame_0000.png (no pad
      connected in the capture VM, so the diagram is neutral and device fields
      read "—"). NOT YET: the artboard's 4-column mapping reference grid with
      Rebind on the focused card - the app still has its older 3-column rebind
      mapping editor below the diagram (untouched, still functional). That
      rework touches rebind capture/persistence and is slice 2.
      Slice 2 (2026-09-06): the mapping editor below the diagram is a full
      functional rebind UI (per-control BtnMap_Click bind buttons, deadzone
      sliders, config profiles, save) - the artboard's compact read-only grid
      would DROP those features, so it was not swapped wholesale (that is a
      functional decision, not a re-skin). Instead the editor was aligned to
      the artboard's flat card aesthetic without losing function: its container
      moved from the translucent CardBackgroundBrush + small corners to
      ThemeRaised + ThemeCornerL, and its 11 inner section cards from ThemeRaised
      to ThemeSurface with ThemeCornerM, so the whole Input tab reads as one
      screen. Built clean (0 warnings), ratchet unchanged, tab renders
      (artifacts/runtime/SHELL_20260906_152303/frames/frame_0000.png; editor is
      below the fold). Full grid redesign that preserves deadzones/profiles/save
      remains open if wanted.
    - [x] **Concept screens recreated in Figma** - DONE 2026-09-06. On request,
      regenerated the shell's screens as native, editable Figma frames (not
      image exports) in a new file (PCSX5 Console Concept,
      figma.com/design/hcglWG0tSW6pUQGAcCzl4l): Library, Tools, Boot analyzer,
      Folder picker, Input, Settings, Console, Error, Booting, Pause overlay,
      Stuck notice - all auto-layout, void-console palette, Inter as the
      stand-in for Space Grotesk / IBM Plex (swappable). Built via the Figma
      MCP (use_figma). This is a design asset, not shipped code.
      Follow-up: the user noted the canvas has 16 real screens (18 artboards
      minus the 2 low-fi direction sketches) vs 11 built, so the remaining 5
      were added - Library all-games grid, Console docked right / bottom /
      floating, and the in-game Quick settings overlay - for 16 total. The
      Figma Starter plan's MCP tool-call limit was hit on the final
      verification screenshot; every build call itself returned node ids.
      Input sub-tabs (2026-09-06): designed two new artboards splitting the
      Input tab into Configuration (rebind grid + deadzones/profile/lightbar/
      save) and Testing (live pad diagram, live button/stick readouts, device,
      motion, test buttons); both added to the concept canvas and published
      (launches focused on Configuration). The matching Figma frames could NOT
      be added - the Figma Starter plan's MCP tool-call limit is exhausted and
      refused the call; the build script is ready to run once the quota resets
      or the plan is upgraded. Two sub-tabs still to be BUILT in the shell:
      Configuration = the mapping editor, Testing = InputTabView's live pad /
      device / motion / tests.
      BUILT in the shell 2026-09-06: ControllerView now has a sub-tab bar
      (SubTabButtonStyle - text label with an accent underline when
      Tag="active") between the title and the content; SetInputSubTab(bool)
      toggles InputTab (Testing, live pad + device + motion + tests) against
      InputConfigView (Configuration, the full mapping editor) and starts/stops
      the live-pad polling accordingly. Lands on Testing; Configuration is one
      click away and keeps ALL its controls (rebind buttons, deadzones,
      profiles, lightbar colour, Save/Apply/Restore) - no functionality lost.
      Two new keys (input.subtab_config/test) in all eleven locales. Built clean
      (0 warnings), ratchet unchanged. Seen: Testing
      artifacts/runtime/SHELL_20260906_154225/frames/frame_0000.png,
      Configuration SHELL_20260906_154259/frames/frame_0000.png.
      Configuration rebuilt to the artboard grid 2026-09-06 (the earlier
      "rebuild to artboard grid" choice): the old 3-column editor became a
      4-column grid of binding cards (MapCardStyle rings accent when a card's
      bind button is focused; card = control label + the binding on a borderless
      mono MapValueStyle button) beside a tuning sidebar (deadzones, profile,
      active gamepad, lightbar + haptics, Restore/Apply/Save). All 39 x:Named
      controls preserved with handlers (26 BtnMap_Click buttons, 4 deadzone
      sliders, config/per-game/active-gamepad combos, 3 colour sliders +
      override + swatch, RestoreDefaultMappingsBtn); the old centre DualSense
      image (not x:Named) was dropped. 31 new keys (input.map.* labels +
      header/hint, input.tune.deadzones/profile/lightbar) in all eleven locales;
      net hardcoded XAML strings went down, ratchet unchanged. Built clean.
      Seen: artifacts/runtime/SHELL_20260906_155927/frames/frame_0000.png.
    - [x] **Settings (side-section list)** - DONE 2026-09-06. The old 3-layer
      hub (grid of category tiles -> category rows -> sub-page) became the
      concept's side-section layout: a persistent left nav rail (SETTINGS title
      + the 8 sections via SettingsNavButtonStyle, the active one on a soft
      accent fill) in a new column 0, with the selected section's rows on the
      right (column 1). The legacy SettingsHubView is kept in XAML but collapsed
      and never shown. Reuses everything: SettingsNav_Click ->
      OpenSettingsCategory builds the rows (PopulateCategoryRows) and highlights
      the nav (CommandParameter holds the section, Tag the active flag);
      sub-pages still open on the right; Save still works (ResetSettingsView now
      re-opens the active section instead of the hub); Back leaves Settings.
      Footer hint "Back to Settings Hub" -> "Back". Built clean (0 warnings),
      ratchet unchanged. Seen:
      artifacts/runtime/SHELL_20260906_161854/frames/frame_0000.png.
    - [x] **Console panel** - polished 2026-09-06 toward the concept. The
      dockable GameConsolePanel already provides the concept's right / bottom /
      left / floating variants (ConsoleDockMode_Click); its log area now uses the
      darker ThemeGround terminal ground with a rounded ThemeCornerM card, matching
      the concept's console. A command-input line was deliberately NOT added -
      the emulator console has no command processor, so it would be a fake
      control (Rule 04). Built clean.
    - [x] **Input Testing polish** - DONE 2026-09-06 from user screenshots: the
      "Not connected" transport chip was a full stadium pill (ThemeCornerPill)
      and read as an oval - now ThemeCornerM like every other chip (standing
      rule saved to memory); the pad no longer sits in a raised card but
      directly on ThemeGround to match the emulator background; it fills the
      available height and centres (MaxHeight cap removed, Testing and
      Configuration now share one star row so whichever is shown fills).
      Global dark ComboBox + ComboBoxItem templates and a thin dark ScrollBar
      template replaced the WPF defaults (unreadable light dropdown, chunky
      scrollbar) app-wide. Seen:
      artifacts/runtime/SHELL_20260906_190915/frames/frame_0000.png.
    - [ ] Remaining screens to rebuild: Error, pause overlay, quick
      settings, first-run setup.

- [~] **3D DualSense in the Input Testing tab, animated by live input** -
  asked and BUILT 2026-09-06; rendering, textures, lightbar and live tilt
  VERIFIED on screen, the per-part press/hinge directions await the user's
  hand test. Model: Sketchfab "PlayStation 5 Dualsense" by AHarmlessPotato,
  CC-BY-4.0 (user supplied the glTF zip; vendored as assets/gamepad/
  dualsense3d with LICENSE.txt + README carrying the required credit).
  Pipeline (headless Blender 5.1, no MCP needed - `blender --background
  --python`, scripts in the session scratchpad, documented in the README):
  import glTF -> apply transforms -> parent_clear (drop Sketchfab's root
  empties so world == local) -> separate by loose parts (120 islands) ->
  classify by bbox anchors into 18 named parts (body, 4 face buttons, 4 D-pad
  arms, 2 sticks, L1/R1, L2/R2, touchpad, PS, mute) -> decimate (112k ->
  47k tris) -> one OBJ per part exported with IDENTITY axes so OBJ and
  manifest share one frame (+X right, -Y face, +Z top edge) + baseColor/
  emissive textures at 1024 + manifest.json with bboxes. Shell: ObjLoader.cs
  (per-usemtl meshes, V flipped) + ControllerVisualizer3D.cs (WPF Viewport3D,
  no new dependency) hosted in InputTabView over the 2D canvas (kept as
  fallback when assets are missing); Pad3D.Update runs from the existing 16 ms
  PollOnce. Animations: whole-pad tilt from the accelerometer, L2/R2 hinge
  about their top edge from the analog axes, L1/R1 click, face/D-pad/PS/
  touchpad depress into the face, sticks lean about their base from
  Lx/Ly/Rx/Ry, lightbar = 1011 emissive tinted by the Configuration colour
  sliders (CtrlColorSliders_ValueChanged -> Pad3D.LightbarColor).
  Two defects found and fixed on the way: (1) the OBJ export wrote world
  coordinates under Sketchfab's parent empties while the manifest measured
  local ones - pivots were off and the pad rendered edge-on and tiny (fixed by
  parent_clear + identity-axis export; verified 577/577 btn_cross vertices
  inside their bbox); (2) VERIFIED in dualsense_ds5w.cpp that the core hands
  over the RAW int16 accelerometer, not g, so a fixed gain saturated at the
  clamp and turned sensor noise into thrash ("moving all over the place" with
  the pad flat) - tilt now comes from the gravity vector via non-degenerate
  atan2 (unit-free), low-passed (k=0.12) with a 1.5 deg deadband.
  build_release.ps1 now stages assets/gamepad (2D and 3D): a dist app had been
  finding the REPO copy via InputTabView's five-parent walk-up, so a shipped
  dist was not self-contained. Seen: face-on, textured, lightbar glowing, live
  accel tilt - artifacts/runtime/SHELL_20260906_194914/frames/frame_0000.png.
  Follow-ups 2026-09-06 from the user's hand test:
  - "D-pad and Triangle/Cross/Square/Circle are missing": the geometry was
    there (each part ~940-2340 tris) - the 1001 texture atlas those parts use
    has alpha down to 24 in exactly their regions, and WPF DiffuseMaterial
    honours brush alpha, so the caps rendered ~90% transparent and read as
    holes. Fixed by flattening the three baseColor textures to opaque (1002/
    1011 were already 240-255). Buttons and D-pad now visible.
  - Testing is now a POPUP (InputTestOverlay, hosting InputTabView) instead
    of an inline sub-tab body, so the pad can be tested without changing
    tabs: while it is open the global pad handler swallows everything (L1/R1/
    L2/R2 included); a PS *tap* is a test like any other button, holding PS
    for 2 s (_psHoldStart) closes it, as do Esc and the Close button. On the
    Input tab with the popup closed, L2/R2 switch the sub-tabs
    (Configuration <-> Testing) and L1/R1 alone cycle the main tabs; L2/R2
    no longer cycle main tabs anywhere. Three keys (input.test_popup_*) in all
    eleven locales. Built clean, ratchet unchanged, dist re-staged 20:03.
    Seen: artifacts/runtime/SHELL_20260906_200201/frames/frame_0000.png.
  Round 3 (2026-09-06, user hand test): sticks and triggers were moving the
  wrong way - both stick lean signs and the trigger/bumper hinge sign were
  inverted (derived from the rotation math, +nx/+ny and +hinge now); pressed
  parts now also GLOW (an EmissiveMaterial per part, accent when down) so an
  input is noticed; the five player-indicator LED dots were re-extracted as
  parts (led_c/l1/l2/r1/r2, with a size guard so the 0.26-wide speaker grille
  sharing their centre does not join them) and light in the DualSense
  player pattern from the pad index (INFERRED pattern), the mute button glows
  orange from MicMuted; the face-button caps are dark GLASS: the atlas's low
  alpha was the intended translucency, so the 1001 texture keeps ~35% alpha on
  the caps and translucent (1001) meshes are drawn LAST with BackMaterial set,
  so the white glyph pieces beneath show through (flattening them to opaque
  had hidden the glyphs - reverted). The Testing SUB-TAB is gone: the Input tab
  is the Configuration editor and a "Test controller" button opens the popup;
  entering the tab no longer opens it; L2/R2 do nothing on the Input tab (and
  still never cycle main tabs). Title music now pauses on every non-Library
  tab and resumes on the Library. The accelerometer graph is normalized to g
  by the pad's resting magnitude on a fixed +/-2 g scale.
  Tilt was then moved off the accelerometer entirely (it is misparsed over
  Bluetooth - next item) onto the gyro: slow bias estimate subtracted, raw
  deadband, leaky integration on pitch/roll/yaw (three axes) that settles
  back to level. VERIFIED level and still with the pad resting:
  artifacts/runtime/SHELL_20260906_202106/frames/frame_0000.png (gyro flat,
  pad face-on, no rocking). dist re-staged 20:22 with all 23 parts.
  Round 4 (2026-09-06, user hand test + photos): (1) the pad still rocked
  and yawed - the gyro integration drifted; root-caused instead to the
  accel/gyro field swap in the core (next item, fixed), and tilt is now
  ABSOLUTE from the gravity vector (roll = atan2(ax, ay), pitch =
  atan2(-az, ay), light low-pass, no yaw): no integration, so nothing to
  drift or settle. Axis signs INFERRED from the SDL/hid-playstation sensor
  frame (+Y out of the face, +Z towards the player). (2) NO glow and NO
  player/mute LEDs ever showed: `EmissiveMaterial.Color` only FILTERS the
  brush, so a black brush with Color=accent stays black - the glow is now
  a SolidColorBrush whose Color is set. VERIFIED: the P1 LED dot lights in
  SHELL_20260906_205955/frames/frame_0001.png and a held R2 glows accent.
  (3) The re-extracted led_* islands were internal geometry 0.24 behind
  the face (removed from the manifest); the five LEDs are now small quads
  synthesized under the touchpad edge at the measured body surface
  (y=-0.306 vs body -0.294, INFERRED position). (4) L2/R2 swung outward:
  hinge sign negated. (5) Mute: the core already maps the mic button to
  bit 0x00200000 - it now depresses, and the mute LED follows a LOCAL
  toggle (lit on first press, dark on the next) OR MicMuted, because the
  reader exposes no LED state. (6) Face-button glyphs were invisible for a
  real reason: the model builds each cap like the real part - a clear top
  disc, walls, and a floor 0.03 below carrying the printed glyph - and the
  low-alpha atlas regions were white BODY panels, not caps (so the 35%
  alpha made the body translucent). Now: alpha opaque everywhere; each
  cap's 1001 mesh is split by depth into the front disc (drawn last as
  smoked glass, opacity 0.88, tinted) and the opaque rest; and the atlas
  has each glyph PAINTED onto the cap's own top disc, oriented through a
  least-squares model->UV affine per disc (so D-pad arrows point along
  their arm). Reproducible from the upstream texture by
  scratchpad tex_round4.py; recorded in the asset README. VERIFIED
  legible: SHELL_20260906_205955 (triangle, square, cross, circle, four
  arrows). dist re-staged 21:0x with 18 parts. OPEN: tilt axis signs and
  the LED pattern remain INFERRED until the user confirms by hand.

- [x] **Round 6 (2026-09-06): Input tab rebuilt to the new artboards; View all
  is the concept's full page; 3D pad share/options, tilt, glass** - design
  first (artifact "Controller Setup v2", three artboards, also kept as
  scratchpad design2/controller-setup-v2.html), then built:
  (1) The Input tab is bindings only: a 6-column grid in a single scroll,
  with "Test controller" and "Controller settings ›" (opens System >
  Accessories & Controllers). The tuning sidebar - sliders whose values were
  never persisted anyway - is gone, and Options (≡) restores default
  bindings (legend updated in eleven locales). Seen:
  artifacts/runtime/SHELL_20260906_220850/frames/frame_0001.png.
  (2) System > Accessories & Controllers gained rows that ARE persisted
  (input.active_slot, input.lightbar, input.per_game_configs): Active
  gamepad (choice, applies lightbar + player LEDs to the pad and the 3D pad),
  Lightbar colour (swatch grid + the colour picker beneath for a custom
  colour, applied to the pad live and at startup; "Pad default" = the slot's
  player colour), Per-game controller configs (toggle), Restore default
  bindings (a new "action" row type). Seen: SHELL_20260906_220902 and
  SHELL_20260906_220915.
  (3) View all now hides the hero and fills the screen like the Library
  artboard; the ring is on the selected tile, not the whole list. Seen:
  SHELL_20260906_220927.
  (4) 3D pad: Share and Options are their own parts (re-exported with two
  new anchors; 20 parts) and depress/glow on bits 0x1/0x8; both tilt signs
  flipped (INFERRED: the user read both axes mirrored, which matches an IMU
  frame rotated 180 deg about Y, not a single-axis error); trigger glow is a
  dim tint instead of a cyan sticker on the black triggers; cap glass is
  near-opaque (0.97) so the floor no longer shows as crescents; all four
  face glyphs share one size (0.032 model units). Seen: SHELL_20260906_220940.
  OPEN: the tilt direction still needs the user's confirmation per axis.

- [x] **Round-5 shell feedback (2026-09-06, user screenshots)** - seven items,
  all built clean, ratchet unchanged, dist re-staged 21:4x:
  (1) Settings option pages (accent colour etc.) were one narrow column in an
  empty page: the option list is now a WrapPanel (280 px items) in a
  stretched container, and level-3 pad navigation is spatial (SpatialNav),
  with Left off the left edge going back. Seen:
  artifacts/runtime/SHELL_20260906_214233/frames/frame_0001.png (two columns
  in the harness's 1200 px window; five on a 1920 px screen).
  (2) Credits popup: the System Information "Credits" row opens an overlay
  showing the README's "## Credits" section, read at runtime from the
  README.md staged beside the exe (build_release.ps1 + csproj Content), so it
  cannot drift from the published list; markdown links/bold stripped, pad
  scroll, Circle/Cross close, its own legend. Seen:
  artifacts/runtime/SHELL_20260906_214136/frames/frame_0001.png.
  (3) Focus auto-scroll: a window-level GotKeyboardFocus handler brings the
  newly focused element into view on every screen (user: focus walked off
  the visible area).
  (4) Input configuration is ONE scroll area (the sidebar and the grid were
  two ScrollViewers with two bars); the three tuning cards use MapCardStyle
  so the card holding the focused slider/picker rings accent like the
  binding cards. Seen: SHELL_20260906_214149/frames/frame_0001.png. Sliders
  adjust with Left/Right, swatches with Cross, the hue strip with Left/Right;
  NOT hand-verified by pad (harness cannot press).
  (5) Library: Options (≡) opens the full library ("View all" was
  mouse-only); the library legend now lists it, localized from ui.view_all.
  (6) Play stops the lobby music (it already resumed on return); new
  setting UI & Personalization > "Lobby music volume" (ui.title_music_volume,
  0..1, default 0.6, applied live), and the title music now uses it instead
  of the game's audio volume.
  (7) Crash overlay: Cross = Boot analyzer, Triangle = Raw logs, Square =
  Copy, Circle = Dismiss, D-pad scrolls the raw text; legend hints.crash in
  eleven locales.

- [x] **Pad navigation on the Input configuration grid is spatial** (2026-09-06).
  The 4-column binding grid was walked as a hand-ordered flat list with
  up/down only, so Down from a card landed "all over the place" (user
  report). New `SpatialNav` (src/ui_csharp/SpatialNav.cs, geometry only):
  from the focused control, a direction picks the nearest control whose
  centre lies that way, preferring ones in the same lane - TV-remote
  movement. Left/right on a focused combo/slider still adjust its value.
  The colour picker's hue slider, swatches and hex field join the set.
  Built clean, ratchet unchanged. NOT yet seen by pad through the harness
  (it cannot press pad buttons); needs the user's hand test.

- [x] **Settings side-nav is reachable by pad** (2026-09-06). The rows-only
  navigation never focused the section column, so the pad was stuck on
  Display & video (user report). Now: Left from the rows (or an unfocused
  row list) focuses the active section; Up/Down in the column switch the
  section and open its rows at once; Right or Cross enter the rows; Circle
  in the column returns to the Library. Same caveat: needs a hand test.

- [x] **Lightbar colour picker replaces the three RGB sliders** (2026-09-06,
  user request). `ColorPickerControl` (own UserControl, Rule 11): a
  saturation/brightness plane (mouse), a hue strip (pad/keyboard/mouse),
  eight preset swatches and a hex field - every route reachable without a
  mouse (Rule 12), four new keys in all eleven locales, AutomationProperties
  on each. Feeds the 3D pad's lightbar as before. Seen:
  artifacts/runtime/SHELL_20260906_210036/frames/frame_0001.png.

- [x] **Core: DualSense accelerometer misparsed on Bluetooth - FIXED 2026-09-06.**
  ROOT CAUSE: DualSenseWindows reads its `accelerometer` from report offset
  0x0F and its `gyroscope` from 0x15, but the DualSense report carries the
  GYRO at 0x0F and the ACCELEROMETER at 0x15 (INFERRED from the Linux
  hid-playstation `dualsense_input_report` layout: buttons[4], reserved[4],
  gyro[3], accel[3]). It is not a Bluetooth-only bug - the swap is the same
  on USB; it only *looked* like a BT layout problem. Fixed at the one
  consumption point, `src/gpu/dualsense_ds5w.cpp` ServiceSlot (the vendored
  library is untouched), by assigning the swapped fields. VERIFIED with the
  pad resting: artifacts/runtime/SHELL_20260906_205805/frames/frame_0001.png
  shows the accelerometer as three flat lines (a steady gravity vector) and
  the gyro as noise around zero - the signature the two fields should have.
  The 3D pad's tilt is back on the accelerometer (gravity, no drift) as a
  result. Original observation kept below for the record. Was: OBSERVED
  2026-09-06 in the Input testing popup with the pad resting on a table over
  Bluetooth: the gyro traces are flat (the pad is not moving) while all three
  accelerometer channels swing roughly +/-2 g every sample
  (artifacts/runtime/SHELL_20260906_201743/frames/frame_0000.png; the earlier
  autoscaled trace SHELL_20260906_195330 shows the same). No physical IMU does
  that at rest, so the accelerometer bytes are being read wrongly. HYPOTHESIS:
  the Bluetooth input report has a different layout/offset from the USB one
  and the accelerometer is decoded at the USB offset (the gyro landing right
  by luck), in DualSenseWindows' parse or in src/gpu/dualsense_ds5w.cpp which
  copies in.accelerometer.x/y/z straight through. Not investigated; needs a
  raw-report capture over BT vs USB and the DualSenseWindows report parser
  read. UNTIL FIXED the 3D pad's tilt is driven from the gyro (bias-removed
  leaky integration), not the accelerometer, and anything else that trusts
  AccelX/Y/Z over Bluetooth is suspect.

- [ ] **4.14 Dreaming Sarah crashes when launched from the shell but not
  from the CLI harness** - OBSERVED 2026-09-06, high blast radius (the user's
  Play button). Launched from the shell (pcsx5_cli with
  `--config-dir --title-id --ipc-map --ipc-pipe --headless <eboot>`),
  PPSA02929 died ~15 s in with a call through a null pointer:
  `Exception 0xC0000005 (Execute at 0x0), RIP 0x0, thread 11888, host stack
  pcsx5_core.dll+0xDC8BD / +0xDBCFA` (build/bin/Release/logs/crash_log.txt,
  05:00; shell frame artifacts/runtime/SHELL_20260906_045913/frames/frame_0034.png
  shows the crash overlay with exit 0xFFFFFFFF). The same title through the
  harness minutes later, PPSA02929_20260906_050230, is `unchanged` against
  the baseline (rendering, no fatal). So the difference is the shell's
  launch path - the IPC/headless presentation, the config dir it passes, or
  the title-id overrides - not today's core changes. Not investigated yet;
  it is a core/IPC boundary, not a UI change, so it gets its own iteration
  (skill: analyze-crash, starting from the two host offsets). Until then the
  pause overlay (4.13 step 9) cannot be verified visually.
  Investigated 2026-09-06 (loop tick), established so far:
  - FALSIFIED: "the shell's config causes it". The CLI launched by hand with
    the shell's exact --config-dir/--title-id/--headless (no IPC) drew 17
    draws and flips per frame for the full 50 s (.work/shellcfg_run.log);
    the only config differences are audio backend 1 vs 0 and a missing
    per-title override.
  - The shell's config dir had no titles/PPSA02929.json, so the harness's
    per-title override (cpu.affinity_mask 3, the two-core pin) was NOT
    applied under the shell. The 05:00 null call happened unpinned; that is
    consistent with a race the pin masks, but it is one observation
    (OBSERVED, not reproduced). The override is now copied into the shell's
    dev config dir.
  - With the override, the shell launch did not crash: the core drew (it
    compiled shaders into build/bin/Release/.work/draw_spv at 05:10-05:13),
    but the shell showed the boot overlay stuck at "Step 1 of 6, 15%" for
    40 s with the "Game may be unresponsive" banner
    (artifacts/runtime/SHELL_20260906_051055/frames/frame_0020.png) - no
    frame ever reached the shell. Pressing Esc on that banner is Force Stop,
    and the shell then reported the kill as "CRASH DIAGNOSTICS ... exit
    0xFFFFFFFF" (frame_0038.png): a UI defect - a user-forced stop is not a
    crash and must not be shown as one.
  - FALSIFIED: "frames are wider than the 1920 IPC cap and get skipped".
    IPC::WriteFrame clamps to 1920x1080 and still bumps the counter, and
    the shell only skips a frame it reads as wider than 1920, which a clamped
    frame never is. Dreaming Sarah composites at 2160x1080, so under IPC it
    would show cropped, not blank.
  ROOT CAUSE FOUND AND FIXED (VERIFIED 2026-09-06): two defects stacked.
  (a) src/ipc/ipc_gpu_bridge.h defined the IPC hook pointers AND their
  setter as C++17 inline - one copy per module. main.cpp (CLI executable)
  called the header-inline setter and filled the executable's copies;
  GPU::RenderFrame in pcsx5_core.dll read the DLL's copies, null forever.
  Nothing in the windowed or in-process paths uses the hook, so only the
  launcher was blind. Fix: the setter is a real function defined in
  vulkan_backend.cpp and exported from the DLL (declared PCSX5_API in
  gpu.h); the header keeps only the pointer variables. (b) In headless mode
  the DIB buffer was never allocated (only window creation allocated it) and
  the headless branch never converted the guest framebuffer, so even with a
  live hook nothing could be published. Fix: allocate on first use, blit
  the guest frame, publish only when a real frame exists. Evidence: the
  core's own log for a shell launch (logging.file_path in the shell's
  per-title override, written to the untracked .work directory): "IPC frame sink registered
  (active)" at startup and "First guest frame published over IPC
  (headless)" at 5.5 s; the shell hid the boot overlay at 13.8 s
  (artifacts/runtime/SHELL_20260906_053150/frames/frame_0012.png). ctest
  gpu/headless/ipc/agc subsets green. No unit test: the defect is a
  cross-module linkage property that a single-module test cannot exhibit;
  the shell run is the test and is recorded here.
  DECISION 2026-09-06 (loop tick): instead of an offscreen renderer for the
  headless IPC mode, the launcher now uses the contract Rule 11 already
  names - the core renders with real Vulkan into a hidden window
  (`--embed` instead of `--headless`) and prints PCSX5_WINDOW_HANDLE=; the
  shell reparents that window. Reasons: it is the documented mechanism, it
  is shell-only, and headless on the Null device can never produce pixels.
  Done and VERIFIED this tick (shell frames SHELL_20260906_054253):
  - IpcSession launches with --embed, parses the handle line into a
    WindowHandle event; GameSession routes it into the same OnCoreWindow
    as the in-process path. The status bar reads "Running" at ~15 s and the
    boot overlay hides on the real window, not on a black frame.
  - FOUND: `_emuHost` (the EmulatorWindowHost) was declared and nulled but
    never constructed anywhere, so EmbedEmulatorWindow returned at its first
    line on every path - the reparent contract was dead code. The host is
    now created on demand when the handle arrives, placed in the presenter,
    and the reparent waits for the HwndHost to own its native window. The
    embedded child now occupies the game area (frame_0014.png).
  - The unresponsive banner was the watchdog starved of heartbeats: only the
    in-process log callback fed it, never the IPC log lines. Both feed it
    now, and the window-ready event does too. No banner in the last runs.
  - A non-zero exit after the user's own Stop/Kill is raised as Stopped, not
    Crashed; the crash dialog no longer appears over a deliberate stop.
  THE LAUNCHER SHOWS THE GAME (VERIFIED 2026-09-06). The last defect: the
  swapchain was rebuilt only on the fullscreen toggle, so once the shell
  reparented and resized the window every present returned out-of-date and
  fell to GDI (core log: "GPU-image present failed" on every frame). The
  windowed present now reads the client size with Win32 each frame and calls
  VkPresentResize (a no-op when unchanged). Core log: "swapchain recreated
  for 1152x675"; the shell shows the Ratalaika splash at 21 s and the
  title's known half-drawn state after -
  artifacts/runtime/SHELL_20260906_054602/frames/frame_0014.png and
  frame_0025.png; 14 unique frames in 26. Two isolated present failures at
  32 s in that log remain unexplained (OBSERVED once; the frame recovered).
  REMAINING, in order:
  1. DONE 2026-09-06: boot phases over IPC follow the core's log markers
     (Loading ELF binary -> Loading, PRX_INIT_QUEUE_END -> Linking,
     GUEST_ENTRY_BEGIN -> Starting CPU, First guest frame -> Running),
     forward only, in GameSession.PhaseFromCoreLine. Seen: footer "Dreaming
     Sarah - Booting (Starting guest CPU...)" at 11 s
     (artifacts/runtime/SHELL_20260906_055052/frames/frame_0014.png).
     Visible consequence to fold into 4.13 step 12 (booting screen): the
     shell hides its overlay on window-ready, so the core's own text-mode
     boot screen shows through inside the shell until the first guest frame;
     the concept's booting screen should own that interval and the embedded
     window be revealed on the first guest frame instead.
  2. Pause from the keyboard while embedded - PARTLY DONE, DECISION NEEDED.
     Done: Esc is now the keyboard's PS tap whenever the shell has focus and a
     game runs (opens/dismisses the pause menu directly, since the pad's tap
     is detected on release and a key press has no held state); elsewhere Esc
     stays Circle. VERIFIED it is not enough: with the game embedded, two
     runs with Esc at 30 s ended on the library with "Ready"
     (SHELL_20260906_055548, SHELL_20260906_055749) - the embedded child
     holds keyboard focus, the core's GLFW handler treats Esc as exit, and
     the shell never sees the key. The earlier "reaches the child" reading
     was right.
     The real question is input ownership while a game is embedded. Today
     the shell's pad tick keeps reading the DualSense through its in-process
     core while the out-of-process core reads the same device: two processes
     on one HID device - the interleaving defect ADR-001 exists to end.
     RECOMMENDED (ADR-001 consistent): while a game runs, the core owns all
     input (pad and keyboard, as it does now); the shell pauses its own pad
     reader; the core reports "menu requested" (PS tap / Esc) to the shell
     through a new word in the IPC block's spare bytes (_pad_end[64]; no
     size change, IPC_VERSION bumped, both sides in-repo); the shell shows
     the pause overlay and sends Stop/Resume commands over the existing pipe.
     Alternative: the shell owns the keyboard and forwards it as pad input
     over the map (WriteInput exists; the core would have to merge IPC input
     into its pad state) - keeps one keyboard owner but leaves the two-reader
     pad problem. Waiting on the user before changing ownership.
  3. DONE 2026-09-06: the core sets the IPC game_state to RUNNING on the
     first guest frame through an exported state sink (IPC_SetStateSink,
     same per-module rule as the frame sink; main.cpp registers
     IPC::SetGameState). OBSERVED at the call site: the core log shows "IPC
     game state -> running (first guest frame)" right after "First guest
     frame presented" (launch SHELL_20260906_060218); the word in the shared
     block itself is not read back by anything yet, so the write is inferred
     from the call, not observed.

- [x] **"Add Directory" on the Game Scan Directories page takes no keyboard
  or pad focus** - FIXED 2026-09-06. The page called btnAdd.Focus()
  synchronously on a just-added element, which WPF drops, so it opened with
  nothing focused. Deferred the focus to DispatcherPriority.Loaded. Verified:
  a keyboard-only sequence (no click) opened the folder picker via Enter -
  artifacts/runtime/SHELL_20260906_064652/frames/frame_0011.png.

- [x] **Compatibility reporting to the external database** - DONE 2026-09-06.
  Standing rule (saved to memory): whenever a title's status is determined in
  PCSX5, record it in both the emulator (compat_seed/titles/<id>.json) and the
  public database (github.com/Abhishekrazy/Pcsx5-Game-Compatibility, one issue
  per title with a status-* label). tools/compat_report.py does both -- updates
  the existing issue's label or creates a new compat-report issue, and writes
  the curated record. Dreaming Sarah recorded as status-boots (issue #1861).
  Also fixed that repo's stats GitHub Action: it opened a non-existent
  COMPATIBILITY_README.md (now README.md), duplicated the Total row and never
  refreshed the date (regex fixes), and installed `requests` on every run (now
  stdlib urllib, nothing to install or cache). The action passes green.

- [x] **Shell fetches community status after a run** - DONE 2026-09-06.
  New CompatDatabase.cs reads a title's status from the public database's issue
  labels (read-only, unauthenticated, every failure returns null so the local
  status stands). MainWindow.RefreshCompatFromDatabase runs when a title stops
  (OnGameStopped): on a non-null result it updates the game's badge if selected
  and persists the tier to compat_seed/titles/<id>.json (source=database), so a
  later launch shows it without a network call. Badge painting extracted to
  ApplyCompatBadge so the hero and the refresh share one path. Built clean
  (0 warnings), ui-strings ratchet unchanged, Library verified in a shell
  capture (BOOTS badge, orange tint).

- [ ] **4.11 UI polish pass** (asked 2026-09-06: "take screenshots and improve
  the UI"). Screenshot every screen of the shell, judge each against the
  standing preference for a PS5-console look, and fix what is wrong: spacing,
  hierarchy, dead space, inconsistent cards, the stale About rows. Each fix is
  its own small change with a before/after capture. Not started until 4.5 is
  seen running.

  - [x] **Stale About rows made honest** - DONE 2026-09-06. The System
    Information page (Settings > About) had four rows typed as "choice" with
    no sub-page, so each opened an empty dead end, and their values were
    invented ("v0.4.2-alpha" while VERSION is 0.1.1, "WASAPI Zero-Copy",
    "Adaptive Trigger support"). They are now read-only "info" rows (no
    chevron, no navigation) with checkable values: the core release reads the
    VERSION file (shipped via the csproj), and the rest name the real
    subsystems without claiming unverified capabilities (Rule 02). Seen:
    artifacts/runtime/SHELL_20260906_065054/frames/frame_0010.png.

  - [x] **Compact binding rows** - DONE. Asked 2026-09-06: "button name and
    their values taking too much space - we can make them like input field
    with label type of look." The twenty label-beside-value rows now use a
    slim BindingValueButtonStyle and small-type labels; the six stacked
    bottom-buttons cells were already compact and are unchanged. Every
    x:Name, Tag and Click is preserved, so the rebind flow and the pad
    navigation list needed no change. Before: artifacts/runtime/SHELL_20260906_015340/frames/frame_0025.png
    After: artifacts/runtime/SHELL_20260906_021648/frames/frame_0025.png
  - [x] **Canvas card capped at 560 px** - DONE. At 1920x1080 the uncapped
    Viewbox scaled the pad taller than the window and pushed the tests and
    the whole mapping editor off-screen. Same after-frame shows all three.
  - [x] **Footer no longer repeats the selected title** - DONE. Asked
    2026-09-06: "from footer remove the selected title name". Selecting a
    tile wrote "Selected: <id>" into the footer status, duplicating the
    header. The write is gone; the footer keeps its real status. Seen:
    artifacts/runtime/SHELL_20260906_024019/frames/frame_0010.png
  - [x] **"View All" is a cover-tile grid** - DONE. Asked 2026-09-06: "view
    all games should show games in grid view". It was a five-column table.
    Same ListView under the same name, now a wrapping panel of 180 px cover
    tiles with the title over the bottom edge; pad navigation gained
    left/right and row-wise up/down with the column count measured from the
    panel; a mouse click selects, a double-click returns to the shelf. The
    search box beside it was already unwired and still is. Seen:
    artifacts/runtime/SHELL_20260906_024352/frames/frame_0009.png
  - [x] **Shelf shows recently played first** - DONE. Asked 2026-09-06:
    "last played title to first ... all recent played should be shown on
    main menu and other in view all". A focused RecentPlays class keeps a
    per-title last-launch time in recent_plays.json beside config.ini (shell
    state, kept out of the core's versioned global.json). The Library shelf
    lists only titles with a stamp, most recent first, or every title while
    nothing has been played so a fresh install is not an empty shelf; View All
    lists everything in the same order. The stamp is written from
    OnGameStarted, once the core process exists, not at the Play click - a
    launch that fails to start is not a play. Seen: seeded stamp puts Dreaming
    Sarah alone on the shelf (SHELL_20260906_024629/frames/frame_0006.png);
    a real Play from a clean file stamped the launched title
    (SHELL_20260906_024839, recent_plays.json read back afterwards).
  - [x] **Play from a dev build could not find pcsx5_cli.exe** - DONE, found
    while verifying the above. IpcSession.LocateCoreExe walked five levels up
    from src/ui_csharp/bin/Release/net9.0-windows/win-x64, which is src/, not
    the repository root, so the build/bin/Release candidate never matched and
    Play reported "The system cannot find the file specified"
    (SHELL_20260906_024642/frames/frame_0025.png). A six-level candidate is
    added beside the existing ones; the next run launched the core.
  - [x] **Shelf is never sparse; View All has a sort picker** - DONE. Asked
    2026-09-06: "if recently not played anything we can show other titles
    there ... I don't want main menu to look blank; view all should also need
    sorting". The shelf now shows every title with recently played first (the
    earlier played-only filter left one lonely tile). View All gained a Sort
    picker - Recently played / Title A-Z / Title ID / Size - with keys in all
    eleven locales and an AutomationProperties.Name; Triangle cycles it from
    the pad and the hint bar says so in every locale. A first cut crashed at
    startup because the combo's initial SelectedIndex fires SelectionChanged
    before the list exists (SHELL_20260906_025359, NullReference in
    ApplyLibrarySort); guarded. Seen: Triangle switches to Title A-Z and the
    grid reorders - artifacts/runtime/SHELL_20260906_025740/frames/frame_0009.png.
    Not persisted across restarts; say if it should be.
  - [x] **SharpEmu launcher navigation compared** - DONE (asked 2026-09-06:
    "sharpemu UI is better ... controller support is better there, check").
    Read sharpemu_clone (its src tree): SharpEmu.GUI/MainWindow.axaml.cs PollGamepad and
    SharpEmu.Libs/Pad/SdlLauncherGamepad.cs. Its model: SDL gamepad, timer
    poll, edge-detected buttons, stick-as-D-pad at 64/192, hold-to-repeat
    400 ms then 130 ms, row-step from measured layout, L1/R1 pages, Cross
    launches; input ignored while the launcher is not the active window or a
    session is running. Ours already has each of those except one: we did not
    gate on window activation, so after alt-tabbing away the shell kept
    reacting to the pad. Added: ControllerTimer_Tick now returns while
    !IsActive, keeping _prevInputState current and clearing the keyboard
    latches so nothing held meanwhile fires on return. Our repeat is faster
    (90 ms) and our coverage is wider (every tab, rebind, overlays), so the
    remaining gap is polish, not capability - tracked under 4.10.
  - [x] **Harness keys T and F** - DONE. The shell's keyboard-as-pad map uses
    T for Triangle and F for Square; the harness could not send them.
  - [ ] The mapping editor's side columns scroll inside 260 px strips with
    tiny viewports (thumbs visible in the after-frame). A page-level scroll
    for the Controller tab would read better than three nested ones.
  - [ ] The canvas card leaves wide empty margins either side of the pad at
    1920 px; the right-hand panel or a MaxWidth could use that space.
  - [x] **Shelf is exactly five, recent first; View All centred; design pass
    with SharpEmu as reference** - DONE. Asked 2026-09-06: "5 titles should
    always be there in main menu, preference always to recent played ...
    SharpEmu menu navigation and design also superior ... I like our menu
    design but there is far better ... view all titles should be centre
    aligned". No `/design` skill exists in this environment, so the pass was
    done directly from SharpEmu's launcher XAML and styles (sharpemu_clone (its src tree):
    SharpEmu.GUI/MainWindow.axaml, Themes/Styles/Library.axaml,
    Surfaces.axaml), rewritten in PCSX5's idiom. Done: the shelf takes the
    first five of the recency-ordered list; the View All grid is centred; its
    tiles now put the name under the cover (ending the overlap with cover
    art), carry a drop shadow, dim to 0.72 when not focused and lift, scale
    and gain a light ring when selected or hovered, with the title as a
    stand-in when a dump has no cover; the hero's title ID, size and status
    are pills; opening View All selects the current game so a tile is lit
    and pad navigation starts from it. Seen:
    artifacts/runtime/SHELL_20260906_030110/frames/frame_0002.png (shelf,
    pills) and SHELL_20260906_030155/frames/frame_0008.png (View All).
    At the harness's 1200 px width the fifth shelf tile is clipped; at the
    fullscreen width the shell starts in, five fit.
    Follow-up DONE ("go ahead", 2026-09-06): a per-title placeholder brush
    hashed from the title ID sits behind each cover (GameEntry.PlaceholderBrush;
    every local dump has artwork, so it is verified as built and bound, not
    seen); the tile lift, scale and brightening now ease over 110-160 ms
    through storyboards on named transforms; the footer pad legend sits in a
    chip. Seen: artifacts/runtime/SHELL_20260906_031400/frames/frame_0009.png
    (clicked tile lifted and ringed, chip in footer) and frame_0002.png.

- [x] **Harness: `--clicks` clicked screen coordinates, not the window.** FIXED
  `click_at(x, y)` moves the cursor to an absolute screen point, and
  `session.py` never offsets by the rect of the window it located by pid. A
  click schedule therefore lands wherever the window happens to be placed,
  silently. Found while trying to open the Input tab for a screenshot: two
  runs, two misses, no error. Done means: clicks are window-client relative
  when a window is known, and the log line says the screen point it resolved
  to.
  Now resolved against the rect of the window the harness located by pid, and
  the log line prints both: `click (565,40) -> screen (925,166)`. Verified by
  the click that had missed twice landing on the Input tab on the first try
  after the change. If no window rect is known the click is refused and says
  so, rather than landing somewhere else silently.

- [ ] **4.12 Keyboard and mouse as a complete no-controller path** (asked
  2026-09-06: "we also need keyboard and mouse support for our emulator
  because not everyone might have a controller"). Two halves, and neither is
  verified as complete today:
  - *Guest input.* A GLFW keyboard backend exists (25 keys mapped); the
    mapping editor can capture a key for a binding. There is no mouse input
    at all - no mouse-as-right-stick, no mouse-as-touchpad, no mouse buttons
    as pad buttons - and analog stick simulation from keys (the standing
    preference) is unverified. Done means: every pad control reachable from
    keyboard or mouse, sticks simulable, touchpad drivable by the mouse, and
    a title played through to a menu with the controller unplugged.
  - *Shell navigation.* Mouse works; keyboard reachability of every control
    is the Rule 12 requirement and has not been audited. This is the same
    focus model 4.10 needs for the pad, so 4.10 and 4.12's shell half are one
    piece of work: one focus model, three input sources.
  Verify each half with the controller physically disconnected.

- [x] **4.9 Retire `WindowsDualSenseReader.cs`** - DONE 2026-09-06. The
  shell's 767-line C# HID reader and its `WindowsHidNative.cs` P/Invoke layer
  are deleted; every former call site reads or drives the pad through the core.
  What it took: four additive exports (`pcsx5_pad_set_rumble`, `_set_lightbar`,
  `_set_player_leds`, `_set_mic_led`) plus the existing state/count exports;
  the eight output/enumeration sites and the `ControllerTimer_Tick` reader
  moved over with their behaviour preserved (the shell nav bitmask, the
  XInput-style state and the 255/0 trigger threshold are unchanged - only the
  source moved). The `HostGamepadButtons` enum survives in its own file,
  `HostGamepadButtons.cs`, because `CheckRebindInput` derives stored binding
  names from its member names, so those names are configuration identifiers;
  the reader's state structs went with the reader. `CheckRebindInput` itself
  has no callers (grep: definition only) - dead code, noted, not touched.
  The mute button is carried to the shell as core bit 0x00200000, a PCSX5
  extension SCE_PAD never defined; that bit MUST NOT reach a guest, and before
  this change `libpad.cpp` copied the mask unmasked. It now strips everything
  above the SCE range at the one point pad state enters guest memory.
  Test `TestPadGuestNeverSeesInternalBits` (tests/hle_audio_pad_tests.cpp)
  injects the bit through the input replay bot: FAILS without the mask
  (`guest never sees the PCSX5-internal mute bit (lhs=2097152 rhs=0)`), passes
  with it. Both observed 2026-09-06. Native and shell builds 0 warnings, 0
  errors; full CTest green after the two stale doc citations were rewritten.
  Shell seen running from the core: artifacts/runtime/SHELL_20260906_023154
  (Input tab reached by harness click; live graphs, 17 of 19 frames unique). Not yet verified on hardware: driving the
  tabs with L1/R1 through the new path - the user has to press the buttons;
  4.10 covers that.

- [x] **Two CTest cases passed while the process aborted** — FIXED
  `CMakeLists.txt` set `PASS_REGULAR_EXPRESSION` on `guest_syscall_smoke` and
  `guest_tls_smoke`; CMake documents that as *replacing* the return-code check,
  so neither could fail on a crash. Both now run through
  `tools/run_guest_smoke.cmake`, requiring the marker AND a zero exit AND no
  `FATAL:` line.
  - [x] Require a zero exit code as well as the marker
  - [x] Also reject a `FATAL:` line — needed because the emulator printed
        `FATAL: abort() raised (signal 22)` and then **exited 0**
  - [x] **Teardown abort root-caused and fixed.** `g_heartbeat_thread` is a
        static `std::thread`; `sys_exit` calls `ExitProcess`, whose
        `DLL_PROCESS_DETACH` runs its destructor while the thread is still
        parked on its 30s wait. A `std::thread` destroyed while joinable calls
        `std::terminate` — which *is* `abort()`, signal 22. The guest run had
        already succeeded, which is why the abort looked unrelated to it.
        The guest exit path now joins the thread first. Both tests pass
        legitimately; **suite is 52/52 and can now actually fail.**

- [x] **The `menus` progress marker is a false positive** — FIXED
  `session.py` matched the bare substring `"menu"`, so it fired on
  `images/menutitle-sheet0.png` — a filename being loaded, not a menu reached.
  Reported as evidence of progress throughout the recent single-title work.
  - [x] Marker table moved to `tools/game_runner/boot_markers.py`, imported by
        both `session.py` and `tools/autorun.py`, which previously kept separate
        copies "kept identical" by a comment — and both carried the bad entry
  - [x] Retired rather than narrowed: no substring of the current log
        distinguishes "a menu is on screen" from "a file with menu in its name
        was opened". `RETIRED_MARKERS` records why, so it is not reintroduced
  - [x] Baseline updated (documented golden change, Rule 07); a run afterwards
        reports `vs baseline unchanged`, so the loss of a false signal did not
        masquerade as a regression

- [x] **No run has ever produced an import report** — FIXED
  `sys_exit` called Win32 `ExitProcess`, which never returns and so never
  reached `pcsx5_shutdown`, the only caller of `PersistSummary`. 0 of 195
  archived runs contained `import_report.json` though 135 requested one. A guest
  exit hook now writes it before the process goes down.
  Before/after on the same ELF: report written **NO -> YES**.

- [~] **The stub-classification regime is inert**
  `RegisterStubContract` (`hle.cpp:462`) has no production callers, so
  `g_stub_contracts` is always empty and `GetStubContract` always returns
  `UNKNOWN`. Three call sites branch on that classification and none of them can
  ever see anything else.
  - [x] The inventory it depends on now exists. Reports were only written on a
        clean `sys_exit`, which no title run performs — they are killed by the
        harness timeout. A periodic flush (30s heartbeat) writes the import
        report regardless, and a killed PPSA02929 run now yields one:
        **165 stubs**, with a call-count heat map.
  - [ ] Populate contracts, which is Rule 04 contract-recovery work per symbol
        and does not belong in this phase. The inventory above is what makes it
        possible to do it in call-count order rather than arbitrarily.

- [x] **`boot_success` was true for a frozen run** — FIXED
  It was `status not in ("crashed","no-frame")`, so `frozen`, `exited` and
  `ran-headless` all counted as a successful boot. CLAUDE.md is explicit that a
  live process painting one unchanging frame is never reported as success, and
  that `ran-headless` means frame validation was impossible by construction.
  Now `status == "progressing"`.
  Two baseline entries claimed success while frozen (PPSA02929, PPSA21564);
  both corrected as a documented golden update.
  - [x] Stamp a `classifier_version` into every record and baseline entry, so a
        record judged under older rules is distinguishable rather than silently
        keeping its verdict

- [x] **Baseline promotion trusted a single sample** — FIXED
  `baseline --update` passed no stability study, and the three stability flags
  defaulted to `True`, so one run of a title whose status varies between crashed
  and frozen was written down as having a stable status — and every later
  comparison trusted it. They now default to **False**: one sample cannot
  establish stability, and only `measure` can.
  - [x] Refuse promotion from a record the current classifier did not judge.
        Verified: an unversioned record is refused with exit 1; a current one
        promotes normally.
  - [x] Baseline re-established for PPSA02929 from 3 samples — status frozen,
        `boot_success` false, stability flags earned rather than assumed.

- [x] **The run classifier is now locked against stored runs** — FIXED
  "progressing" is the only verdict this project treats as success, and it rested
  on four thresholds with no test. The risk is not a bug but an edit: loosening
  one to turn a red run green.
  - [x] Classifier extracted into a pure `classify_progression`, so it can be
        replayed over stored records rather than only over live runs
  - [x] `tools/check_run_classifier.py`, registered as the `run_classifier`
        CTest: replays **179 stored runs**, requires known-stuck runs to be
        rejected, and requires every threshold to be load-bearing
  - [x] Proven to catch an edit: quietly loosening `DISTINCT_RATIO_MIN` from
        0.5 to 0.05 turns it red; restoring turns it green
  - [x] No new dependency — plain Python and CTest, matching
        `check_nid_registrations.py` and `check_doc_links.py`, rather than
        adding pytest
  Note: `COVERAGE_MIN` gets a synthetic fixture rather than a corpus assertion.
  Every archived record predates `capture_failures`, so all replay at 100%
  coverage and the corpus cannot exercise that guard at all.

- [x] **Documentation paths cited by CLAUDE.md did not exist** — FIXED
  Seven paths every session is told to read led nowhere. `docs/tasks`,
  `docs/audits`, `docs/walkthroughs` and `docs/evidence` were absent entirely,
  while `RUNTIME_LIFECYCLE.md` and `PS5_BOOT_PIPELINE.md` live under
  `architecture/`. Rules 03/04/07/09 all discharge into "the owning audit",
  so those rules were unenforceable and nothing said so.
  - [x] Four directories created, each with a README stating its purpose
  - [x] References repointed across 12 files
  - [x] `docs/templates/{TASK,AUDIT,WALKTHROUGH}.md` created — the evidence
        skill cited templates that did not exist
  - [x] A cited audit that was never written is now a pointer to `docs/audits/`
        rather than a ghost filename
  - [x] `tools/check_doc_links.py`, registered as the `doc_links` CTest: 216
        referenced paths, all resolve. Proven to fail — a deliberately broken
        link makes it red, and removing it makes it green again.

---

## Resolved — the three items the first stub inventory raised

All three were investigated and none was a defect. Kept with their evidence so
they are not re-opened.

- [x] **`_Getptolower`, the hottest call in the emulator** — not a defect. It is
  implemented at `libkernel.cpp:2088` and builds its table exactly once under
  `std::call_once`; every later call returns a cached address. It is hot because
  the guest's Dinkum CRT calls it per case-insensitive operation, which a
  Construct engine parsing JSON does constantly. The only cost on our side is
  HLE dispatch, which belongs to the general performance task, not here.

- [x] **`9BcDykPmo1I`, "an unresolved NID called 308,960 times"** — my record was
  wrong. It is `__error`, the BSD per-thread errno accessor, it was already in
  `assets/nid_db.txt` at line 169, and it is implemented at
  `liblibc.cpp:2569`. Every `errno` check in the guest's libc calls it, so the
  count is expected.

- [x] **`5OqszGpy7Mg` is `strtoull`** — now VERIFIED rather than inferred from a
  matching call count. Recomputing the NID of "strtoull" yields `5OqszGpy7Mg`
  exactly; the same computation reproduces `_Getptolower` and `__error`, which
  checks the algorithm itself.

### The real defect underneath them

- [x] **The inventory printed raw NIDs for the symbols nobody can recognise**
  `ParseNidString` requires 11 encoded characters PLUS a 4-character tag, so a
  symbol registered as a bare NID never parsed and never resolved — which is
  exactly why `__error` read as an unknown and was recorded as one. The report
  path now decodes a bare NID too, and accepts it only when the table knows the
  value, so an 11-character *name* is left alone rather than renamed into
  something wrong. Three verified `strto*` NIDs added to the database.
  Top ten went from three opaque NIDs to none; 36 of 165 still show raw NIDs,
  which is honest — those names are genuinely unknown.

## Discovered — cached textures are never invalidated

- [ ] **A texture is cached by its descriptor, never by its contents**
  `TextureIdentity` (`src/gpu/vk_draw.cpp:403`) hashes guest address, width,
  height, pitch, format, tile mode, mip count and array shape — and nothing
  about the pixels. If the guest rewrites the data at that address with the same
  dimensions and format, the key is unchanged, the lookup hits, and the old
  image is displayed for the rest of the run. Nothing invalidates the entry: the
  only eviction is capacity (`> 512` entries clears everything).
  Blast radius: **many titles**. Anything that updates a texture in place —
  render-to-texture, animated or procedural textures, streamed atlases, dynamic
  UI, video-on-polygon — shows its first frame forever.
  **Both** reference emulators solve this the same way, arrived at
  independently, which is the strongest signal available that it is the right
  architecture: Kyty's `host_gpu/memoryTracker` + `pageManager` keep dirty
  ranges per page (`ValidateGpuDirtyPages`), and shadPS4's
  `video_core/page_manager.cpp` write-protects pages via
  `Protect(VAddr, size, MemoryPermission)` and invalidates on the fault.
  shadPS4 also carries `multi_level_page_table.h`, as does Kyty.
  - [ ] Confirm a title actually rewrites a texture in place before building
        page tracking — hash contents for small textures as a cheap probe first,
        and measure how often the hash changes for a fixed descriptor
  - [ ] Only then decide between content hashing and write-tracking; page
        protection is the expensive answer and should be justified by a
        measurement, not adopted because the reference has it

## Discovered — from the shadPS4 comparison

shadPS4 runs commercial PS4 titles, and PS5 shares most of the Orbis API
surface, so where it differs from us the difference is usually load-bearing.

- [ ] **Neither reference has a "targetless draw" concept — because both decode
      the colour-target registers**
  shadPS4 keeps `color_buffers[NUM_COLOR_BUFFERS]` in its register state
  (`video_core/amdgpu/regs.h:150`) with a full `ColorBuffer` layout
  (`regs_color.h:116`), and Kyty decodes the same registers. Our
  `pending_targetless` path exists only because `DecodeRenderTarget` never sees
  CB_COLOR0 — which is Phase 2's second task. This raises its priority: the
  fallback is not an architecture, it is a symptom, and no working emulator
  needs one.

- [ ] **Keyboard and mouse input mapped to the pad**
  shadPS4's `input/input_handler.h:199` defines `string_to_keyboard_key_map`, a
  configurable key-to-button binding, alongside `input_mouse.cpp`. We have no
  keyboard path at all. This is already Phase 3's first task; the reference
  gives it a concrete shape to follow rather than inventing one.

- [ ] **Audio decode is far thinner on our side** — LOW priority
  shadPS4 implements the Audio Job Manager with AT9 and AAC decoders
  (`core/libraries/ajm/`, ~2,137 lines). We have `libatrac9.cpp` at 370 lines
  registering 5 symbols. Recorded for completeness rather than urgency:
  PPSA02929 makes exactly **one** AJM/Atrac call in a full run, because it ships
  `.ogg`/`.flac` and decodes them itself. Worth doing when a title needs it,
  not before.

- [ ] **RECTLIST is mapped to a triangle strip** — latent, affects any title using it
  Kyty carries a dedicated `shader/rectListShader.cpp`. Our
  `PrimitiveTopologyFromVgt` maps VGT type `0x11` (DI_PT_RECTLIST) to
  `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP`. A rect list defines a rectangle per
  three vertices with the fourth corner computed, which a strip cannot express,
  so each rectangle renders as one triangle. Measured **not** to affect
  PPSA02929 — its draws are `vgt_primitive_type=0x4` (TRILIST), verified by
  logging at pipeline creation — so this is a real defect waiting for the title
  that uses it, not a current one.

## Phase 2 - GPU: stop discarding work

- [-] **FALSIFIED: executing every targetless draw makes the picture WORSE**
  Implemented and measured. The queue works mechanically — executed draws rose
  from 207 to 1317, and composites per flip went from always 1 to 9–12 — but the
  screen goes black:

      single slot (before)   37 of 37 captured frames had content
      queue       (after)     8 of 44, black from frame 8 onward
      after revert           39 of 39 had content again

  **Why it goes black is NOT established.** I first wrote that the earlier draws
  are intermediate passes being painted over the finished image. That was
  reasoning, not measurement, and a later probe contradicts it: the 9–12
  targetless draws in a frame are *identical in every respect we can observe* —
  same vertex and pixel shader, same source texture at the same address, the
  same 6-vertex full-screen quad, and the sampled texture's contents hash the
  same across all of them. They are not a chain of passes over changing data.
  A plausible alternative I have not tested: 6x the draw work on a title already
  at ~9 fps may simply stop frames being presented, and the capture then records
  black. Until that is measured, "the queue makes it worse" is the observation
  and the cause is UNKNOWN.
  Reverted (Rule 10) rather than kept and tuned.
  **What this means for the phase:** "stop discarding work" was the wrong frame.
  The draws are discarded because we cannot represent what they render *into*.
  The real question is whether these intermediate passes need offscreen targets
  of their own — which is a much larger design question than a queue, and one
  that should start from what the guest's shaders actually write.
  Draw accounting from `8b3bb45` stays: it is what made this measurable.

- [-] **FALSIFIED: "the colour-target binding is never observed" is not a defect**
  Measured directly by logging every context register the guest sets through the
  indirect patch path. It sets **54 distinct registers and not one of them is a
  colour-target register** — none of `0x318`, `0x319`, `0x31C`, `0x390`, `0x3B0`.
  The full set is shader state only: `SPI_PS_INPUT_CNTL` (0x191–0x1AF), shader
  configs (0x1B1–0x1C5), `CB_SHADER_MASK`, `CB_TARGET_MASK`, and no viewport,
  scissor, blend or raster mode either.
  So the guest genuinely never binds a render target; it composites straight to
  the scanout buffer. `DecodeRenderTarget` returning false is correct, and the
  targetless path is the right path for this title — which is precisely why
  SharpEmu, the emulator ours was modelled on, carries `PendingTargetlessDraw`.
  Separately noted while checking: `kCbColor0BaseHi = 0x319` looks wrong
  regardless. SharpEmu names the high bits `CbColor0BaseExt = 0x390`, and 0x319
  is absent from our own defaults table while 0x390 is present. Harmless today
  because no title in the fleet sets either, but wrong for one that does.

- [-] **ANSWERED: nothing differentiates them. They are duplicate draws.**
  Five independent measurements, each expecting to find the differentiator and
  each finding none:

  | measured | result |
  |---|---|
  | vertex + pixel shader addresses | identical across all draws |
  | source texture address and size | identical (0x215ed0000, 1280x720) |
  | source texture *contents* (hashed) | identical — one hash |
  | vertex buffer address, size, *contents* | identical — one hash |
  | `DRAW_INDEX_OFFSET_2` index offset | **0 on every draw** |

  So the 9–12 targetless draws per frame are byte-for-byte the same draw. The
  single slot loses nothing, executing one is equivalent to executing twelve,
  and the "87% discarded" figure counts redundant work rather than lost content.
  Phase 2's premise — *stop discarding work* — is retired: there is no work to
  recover.

  **And the draws are identical because the game is frozen.** It is redrawing an
  unchanging frame, so of course every draw matches. This whole line of
  investigation was measuring a consequence of the stall, not a cause. That is
  the useful result: the GPU path is not what is holding this title back.

  Two real defects were found along the way and are recorded separately —
  the texture cache never invalidating, and `kCbColor0BaseHi = 0x319`.

- [x] **DRAW_INDEX_OFFSET_2's first-index is now read** — IMPLEMENTED, not VERIFIED
  Our packet emitter writes `index_offset` at `packet+8`; the walker never read
  it back, so every such draw rendered from index 0 whatever slice the guest
  asked for. A title batching sprites into one index buffer and walking it by
  offset would draw its first primitive repeatedly.
  `ResolvedIndexAddr` now advances the index base by the first-index, and the
  snapshot starts there so the copied range is exactly the indices drawn.
  **No test.** One was written and abandoned: exercising it through
  `sceAgcDriverSubmitDcb` segfaults `hle_agc_tests`, because that path reaches
  real GPU work the test harness has no device for. A crashing test is worse
  than none, so it was removed rather than committed. Covering this needs a way
  to drive the walker without executing draws — worth building, and it would
  cover several other AGC behaviours at once.
  **No runtime evidence either:** every `DRAW_INDEX_OFFSET_2` in PPSA02929
  carries offset 0, measured across a full run, so the title cannot show the
  difference.

- [x] **CB_COLOR0 high address bits read from the right register** — IMPLEMENTED
  `kCbColor0BaseHi` was `0x319`, which is not a register — it looks like a guess
  at "the one after BASE". The high bits are `CB_COLOR0_BASE_EXT` at `0x390`:
  both reference emulators name it that, and **our own Gen5 defaults table
  contains 0x390 and no 0x319 at all**, which is the local evidence the old
  value was never real. Composition unchanged and matches GFX10:
  `(BASE_EXT << 40) | (BASE << 8)`.
  Unverifiable at runtime: no tracked title sets either register, so
  `vs baseline unchanged` is the most that can be said. ctest 52/52.

- [ ] **A way to drive the PM4 walker without a GPU**
  Testing anything in the walker currently means `sceAgcDriverSubmitDcb`, which
  reaches real GPU work and segfaults `hle_agc_tests`. Four routes around it were
  tried and all crashed — mapped index buffer, memory inside the command buffer,
  a zero index count, and recording state at parse time rather than at
  execution. `VkDrawExecute` is documented as a safe no-op without a device, so
  the crash is elsewhere in the submit path and has not been located.
  This blocks tests for the index-offset fix and for every other walker
  behaviour, so it is worth more than any single one of them.
  *Note:* the emulator arguably should not crash when walking a command buffer
  with no GPU present, so this may be a robustness defect rather than only a
  test-harness gap.

- [ ] **Execute or explain the skipped AGC packets** - NOP `0x19` (DMA data),
  `IT_EVENT_WRITE` (`0x46`), op `0x76`.

---

## Phase 3 - Input, and the rest

- [x] **CORRECTED: the input path already existed. `--play-input` was the gap.**
  I repeatedly claimed the core had "no keyboard input path at all — no
  `GetAsyncKeyState`, no `WM_KEYDOWN` anywhere in `src/`". That was wrong: I
  grepped for the wrong APIs. The window is GLFW, so keyboard input goes through
  `glfwGetKey`, and `src/gpu/input/glfw_keyboard_backend.cpp` maps 25 keys.
  There are **six** input backends — keyboard, DualSense, XInput, SDL
  GameController, a multiplexer, and `input_bot.cpp`, which synthesises
  controller state from a JSON replay. `--record-input` and `--play-input` are
  both documented in `--help`.
  **The real defect:** `play_input_path` was declared in `core_api.h` and set in
  `main.cpp` and **read nowhere**. The flag was accepted, advertised, and did
  nothing — the same silent-success class as the rest of this project's worst
  defects. Now wired: replay supersedes live devices, so a replay-driven run is
  reproducible and does not also pick up whatever is plugged in.
  Verified both ways: a valid replay logs `InputBot: loaded 3 events (120
  frames)` and `Input replay active`; a missing file logs the failure and says
  live input is unchanged rather than pretending.

- [x] **Drive the harness from replays** — DONE, and it needed no harness change
  `session.py` already had `--input`, which passes `--play-input=`, plus
  `--keys` and `--clicks` schedules. The pipeline was built end to end on the
  harness side and was dead only because the core ignored the flag.
  Verified: `session.py run --title PPSA02929 --input <replay>` logs
  `InputBot: loaded 3 events (120 frames)` and `Input replay active`.
  Worth noting what this means about the earlier defect — someone built the
  harness expecting replay to work, and it silently did not, for as long as the
  option went unread.
  **Still unverified at the guest level:** no tracked title calls
  `scePadReadState`, so nothing yet proves an injected button reaches guest
  code. Closing that needs either a title that polls the pad or a guest test ELF
  that does. **The guest-ELF route is not cheap**, which is worth writing down
  before someone reaches for it: `tests/test_elf/*.cpp` are freestanding clang
  binaries that talk to the emulator through raw `syscall` instructions. They
  have no PS5 import table, so they cannot reach `scePadReadState`, which is
  resolved by NID through `PT_SCE_DYNLIBDATA`. Using one as a pad instrument
  means hand-building a PS5-shaped import table first.

- [x] **The pad read path reported success for writes the guest never got**
  `scePadReadState` and `scePadRead` filled the guest's buffer through
  `Memory::WriteBuffer`, which is guarded but returns `void`. A buffer the
  emulator could not write to therefore produced a cheerful `0` from
  `scePadReadState` and `1 entry read` from `scePadRead`, while the guest kept
  whatever bytes were already in its buffer — a controller frozen mid-state,
  with no error to test and nothing logged where it broke.
  The memory subsystem was already detecting it and saying so
  (`GuardedWrite: invalid write at 0x... (copied 0 of 120 bytes)`); the pad code
  simply ignored the answer. That is the whole defect, and it is the same shape
  as `MemcpyImpl` and as `--play-input` being accepted and unread.
  Both now check the guarded write. `scePadRead` additionally counts only
  entries the guest actually received, so failed writes no longer consume ring
  samples the guest never saw.
  Test: `TestPadRejectsUnwritableBuffer` in `tests/hle_audio_pad_tests.cpp`
  maps a `PROT_READ` page and passes it in. Before: 2 failures (`lhs=0` for
  ReadState, `lhs=1` for Read). After: suite passes, with
  `scePadRead: entry 0 unwritable at 0x... (0 of 120 bytes); returning 0`.
  INFERRED, and flagged as such in the code: returning the invalid-argument
  error for an *unwritable* rather than null buffer matches the null case by
  analogy. What retail firmware does here is UNKNOWN — on hardware the store
  would most likely fault inside the guest rather than return at all.

- [x] **`scePadGetData` wrote through a raw, unguarded host store** — FIXED
  `src/hle/libscepad.cpp` zeroed 64 bytes of the guest's buffer with a loop of
  `Memory::Write<u64>`, which is `*reinterpret_cast<T*>(addr) = value`: no page
  check, no demand-commit, no failure report. Rule 05 forbids exactly this.
  Evidence that it mattered: a `PROT_READ` guest page is genuinely host
  read-only — a plain `memset` to one terminates the process (exit 139) — so
  this store was writing into memory whose protection nothing had consulted.
  Now a checked `GuardedWrite`. Before: `GetData(unwritable buffer)` returned 0,
  reporting success (`lhs=0`). After: returns the invalid-argument error and
  logs `scePadGetData: buffer 0x... unwritable (0 of 64 bytes)`.
  **Two things deliberately left alone**, both noted in the source so they are
  not mistaken for settled: the 64-byte length is inherited and probably wrong
  (`ScePadData` is 0x78), but fixing it needs a caller observed at runtime, not
  a guess made while fixing something adjacent; and the error code is INFERRED
  by analogy with the null-pointer case — what retail firmware returns for an
  unwritable buffer is UNKNOWN.

- [x] **`Memory::WriteBuffer` retired in favour of `GuardedWrite`** — DONE
  The user chose removal over `[[nodiscard]]`, and removal proved the better
  call for a reason worth recording: the compiler then enumerated every call
  site, so none could be missed. It found 26 in `src/` **and two in `tests/`**
  that a `src`-only search had not — including `memory_validation.cpp`, which
  was round-tripping through the very wrapper being removed.
  Conversions were not uniform. Two were more than mechanical:
  - `kernel.cpp` emulates guest TLS stores inside the VEH: it wrote, advanced
    RIP, and resumed. A failed store meant the guest resumed as though it had
    happened, and its next read of that slot returned a stale value with nothing
    recorded. It now refuses to resume on a store that did not land.
  - `hle.cpp` wrote a thunk's machine code and returned the address regardless,
    handing the guest a pointer to whatever happened to be there. Now returns 0.
  The rest are out-parameter writes returning their module's existing
  invalid-argument error instead of a plausible success. No error codes were
  invented.
  The tests were strengthened, not weakened: `memory_validation` now asserts
  `GuardedWrite`'s return value and byte count, including across a page
  boundary — an assertion the `void` wrapper made impossible.
  `memory.h` carries a note saying what was removed and why, so the wrapper is
  not reintroduced as a convenience.
  Verified: 51/52 ctest (only the unrelated `doc_links`), 0 warnings,
  PPSA02929 unchanged against baseline.

- [ ] **`Memory::ReadBuffer` has the identical flaw, at 38 sites**
  It wraps `GuardedRead` and discards the result: `kernel.cpp` (13),
  `vk_draw.cpp` (6), `elf.cpp` (5), `libkernel.cpp` (3), `libatrac9.cpp` (3),
  `guest_printf.cpp` (2), and one each in `libkeystone`, `libaudioout`,
  `libagc`, `xa2_device`, `wasapi_device`, `audio_device`.
  Arguably worse than the write case: a failed write leaves the guest with stale
  data, while a failed read hands *our own code* plausible-looking garbage and
  it proceeds on it. `elf.cpp` reading headers and `vk_draw.cpp` reading vertex
  data are the ones to look at first.
  Not started: the user approved retiring `WriteBuffer` specifically, and
  extending that to `ReadBuffer` is their call, not an assumption to make.

- [x] **FALSIFIED: "DualSense vibration, lightbar, mic-mute and analog sticks
  are broken."** Tested against real hardware on 2026-09-05, connected over
  **Bluetooth** (VID 054C / PID 0CE6 via `BTHENUM`), using a new manual probe,
  `tools/dualsense_probe.cpp`. Every one of them works in the core:
  ```
  left  stick x:[3..255] y:[0..255]     right stick x:[0..255] y:[0..255]
  triggers  L2 max=255  R2 max=255      buttons 0x0010FFF0   touch 1 finger
  ```
  and the user confirmed all four outputs physically occurred: lightbar colour
  cycle, both rumble motors, mic-mute LED, player LEDs.
  The earlier report was accurate when made; replacing the guess-the-offsets
  in-header reader with the vendored DualSenseWindows library fixed it. The
  entry is kept, marked falsified, so nobody re-investigates a solved problem.
  The probe is deliberately **not** a CTest: it needs hardware and a person to
  confirm what they saw, so as an automated test it could only pass vacuously.

- [x] **The shell has a second, competing DualSense reader — this is where the
  breakage the user saw actually lives.** DONE 2026-09-06 under 4.9: the reader
  is deleted and the shell reads the core. The finding as recorded:
  `WindowsDualSenseReader.cs`
  (767 lines) is a complete independent C# HID implementation, and the shell's
  Controller Setup uses it — `CoreBridge.cs` exposes no pad state at all.
  Both it and the native reader open the device with
  `FILE_SHARE_READ | FILE_SHARE_WRITE`, so neither blocks the other; they
  **interleave**. Two consumers of one HID input stream each receive a share of
  the reports, and two writers overwrite each other's output state. That fits
  "works standalone, broken in the app" precisely.
  HYPOTHESIS, not yet confirmed: the next step is to open Controller Setup with
  nothing else running. If it works alone, interference is the cause; if it is
  still broken, the C# reader has a defect of its own.
  Either way the duplicate should go (ADR-001), but this decides urgency.
  **Blocked on approval:** exposing pad state through `CoreBridge` is a public
  native ABI change, which CLAUDE.md makes a stopping condition.

- [x] **Battery percentage above 100% — fixed in the vendored library**
  `DS5_Input.cpp` computed `(nibble * 100) / 8` on a 4-bit field, so it could
  report up to 187%. Real readings of 125% and 112% are what exposed it.
  Clamped to 100 rather than rescaled: the true scale of that nibble is
  `UNKNOWN` here, and guessing a divisor would swap a visibly wrong number for
  an invisibly wrong one. Recorded as a local modification with a removal
  condition in `third_party/DualSenseWindows/README.md`, as the vendoring
  policy requires.

- [x] **DualSense haptics over Bluetooth - WORKING, verified on hardware**
  A 3-second tone at 60 Hz, 150 Hz and 30 Hz each produced felt vibration on a
  Bluetooth-connected DualSense. Reproduce: `build/Release/dualsense_probe.exe`.
  Implemented as `PlayHapticsPcmBlocking` in `src/gpu/dualsense_ds5w.cpp`:
  report `0x32`, 142 bytes, s8 stereo PCM at 3000 Hz, 64 bytes per report every
  ~10.67 ms, preceded by an init-prime and 8 silence reports.
  This closes a HARD BOUNDARY I declared and then falsified twice. Full account
  in `docs/audits/AUDIT-2026-09-05-dualsense-audio-over-bluetooth.md`.
  The failure worth carrying forward: the second attempt sent 282 reports, all
  accepted, and did nothing, because the stream was never opened by an
  init-prime. The device returned success for work it discarded - the same shape
  as every other defect this project has hunted, arriving from the hardware
  rather than from our own code.

- [ ] **DualSense speaker audio over Bluetooth - needs a decision, not research**
  A different lane from haptics: report `0x35`, 334 bytes, carrying a 200-byte
  **Opus** frame (48 kHz stereo, 10 ms, 160 kbps CBR - the arithmetic is exact,
  160 kbps x 10 ms = 1600 bits = 200 bytes). There is no PCM speaker lane over
  Bluetooth, so Opus is not a choice; it is what the device decodes.
  Assessed in `architecture/decisions/ADR-002-opus-for-dualsense-speaker.md`.
  **Writing our own encoder was considered seriously and rejected.** A
  conformant CELT-mode encoder needs a bit-exact range coder, Opus's MDCT
  windowing, 21-band energy coding, PVQ with exact V(N,K) indexing, and above
  all the bit-allocation logic that the decoder independently recomputes - any
  divergence desynchronises it completely. libopus is ~50k lines by codec
  specialists. There is no partial credit: 95% correct produces noise, not
  slightly worse audio.
  Recommendation: vendor **libopus** (BSD-3-Clause, GPL-2.0 compatible, no
  external dependencies), following the `LibAtrac9` precedent.
  **Awaiting the user's decision**, because a new dependency is theirs to
  approve. Note the cheapest correct outcome is also on the table: over USB the
  speaker is an ordinary Windows audio endpoint needing no codec at all, so if
  Bluetooth speaker audio is not actually wanted, this dependency should not be
  added.

- [ ] **DualSense microphone input over Bluetooth - still `UNKNOWN`**
  Every source examined covers the output direction only. It is a different data
  path and must not be assumed to work because output does.

- [ ] **`third_party/LibAtrac9` has no README.md, which the vendoring policy
  requires.** Found while checking precedent for the Opus decision. The policy
  in `CLAUDE.md` requires every vendored library to record upstream URL, author,
  pinned commit, licence, GPL-2.0 compatibility, owner, build wiring, update
  procedure and local modifications. `DualSenseWindows` has one; LibAtrac9 does
  not, so its provenance is currently unrecorded.

- [ ] **Wire haptics into the emulator, not just the probe.**
  `PlayHapticsPcmBlocking` blocks for the duration of the audio, which suits a
  test and not a running game. Production use needs a streaming interface fed by
  the guest's haptics output on its own paced thread.

- [ ] **ADR-001 steps 2–4 remain**

- [ ] **A guest-filesystem test fixture** - blocks tests for `feof`/`fgets`/
  `fgetc` and for the guarded-transfer reporters.

- [ ] **Save-data mount returns an empty mount point** - the guest builds
  `/-saveindex` with no prefix; `sceSaveDataSetParam`/`SaveIcon` are stubs.

- [ ] **Performance: ~9 fps against a reference emulator's 60 fps.** Measure
  honestly first: flip rate is not uniform, so flips divided by duration compares
  nothing across runs of different lengths.

---

## Later

- [ ] DualSense: vibration, lightbar, mic-mute, analog sticks not registering,
  misaligned diagram overlays, no live test panel. Needs the user's hardware.
- [ ] Multiple controllers - never exercised.
- [ ] UI ratchet - 229 hardcoded XAML strings, 0 of 119 controls with
  `AutomationProperties.Name`, 0 `DynamicResource`.
- [ ] Committed build outputs under `src/ui_csharp/bin/**`.
- [ ] `tools/dream_tool.py` targets the wrong title's eboot.
- [ ] `GetWindowRect` includes the DWM resize border (~7px);
  `DWMWA_EXTENDED_FRAME_BOUNDS` is correct for the fallback path.

---

## Falsified — kept so they are not re-attempted

- [-] **"The 0xFFFFFC18 condvar timeout is a negative duration we misread"** —
  the reference implementation types it `unsigned int`, waits the same ~71
  minutes, and runs the title anyway.
- [-] **"The half-black quad is a GPU rendering defect"** — the splash renders
  perfectly at frame 12; the diagonal is a wipe transition frozen part-way, a
  symptom rather than a cause. Topology, culling and vertex count were each
  measured correct.
- [-] **"The guest never calls `sceKernelWaitEqueue`"** — it calls it once per
  frame. The wait accounting only counts the *blocking* path.
- [-] **"End-of-pipe release-mem is never executed, so the guest waits forever"**
  — the packet is genuinely unhandled, but its data-selection field is zero on
  every occurrence, so no write is requested and skipping it is correct.
- [-] **"The title is merely slow, not stuck"** — twenty minutes produced no new
  guest output at all.
- [-] **"Background loading stalled at 49 of 65 media files"** — the 16 unopened
  files are music, streamed on demand rather than preloaded.
