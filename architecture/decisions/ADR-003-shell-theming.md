# ADR-003: Shell theming — token palette, light/dark mode, user colours

**Status:** Accepted, 2026-09-06
**Owner:** desktop shell (`src/ui_csharp/`)

## Context

The shell ships a single fixed dark palette in `App.xaml`, and nearly every
colour in `MainWindow.xaml` is an inline literal (`#0099FF`, `#FFA0A0A5`, ...).
Rule 12 records the consequence: the shell cannot follow a light or
high-contrast theme, and it forbids shipping a second parallel theme system
without an ADR. On 2026-09-06 the user approved a from-scratch redesign of the
shell (the "Void console" concept) and asked for two things beside it: a light
and dark theme mode, and theme colours the user can change from Settings.

Rebuilding screens on literals again would make both impossible. The palette
has to become the one place colour is decided.

## Decision

1. **One token palette, defined once.** `App.xaml` declares a fixed set of
   named brushes (`ThemeGround`, `ThemeSurface`, `ThemeRaised`, `ThemeHairline`,
   `ThemeText`, `ThemeTextMuted`, `ThemeAccent`, `ThemeOnAccent`, `ThemeDanger`,
   `ThemeWarning`, `ThemeSuccess`). Every screen built from now on references
   them with `DynamicResource`, never a literal and never `StaticResource`.
   Existing screens keep their literals until each is rebuilt; the ratchet in
   `tools/check_ui_strings.py` measures that no new literal is added.

2. **A focused `Theme` class owns the values.** `src/ui_csharp/Theme.cs`
   holds two built-in palettes (dark, light), resolves `system` from Windows'
   apps-theme setting, applies the user's accent (and optional ground colour)
   on top, and writes the resulting brushes into `Application.Resources` at
   startup and whenever a setting changes. `DynamicResource` makes the change
   live without a restart. No other code writes those resources.

3. **Settings own the choice.** `EmulatorConfig.ui` gains `theme`
   (`dark` | `light` | `system`), `accent` (`#RRGGBB`) and `ground`
   (`#RRGGBB` or empty for the palette's own). They persist in `config.ini`
   with the other `Ui` keys and are exposed in the Settings screen as
   pad-navigable choices: a mode cycle and a curated swatch cycle for the
   accent, with a hex field for keyboard users. No Windows colour dialog: the
   standing preference is a console interface.

4. **Contrast is a requirement, not a hope.** The two built-in palettes are
   checked once for readable text-on-ground and text-on-surface contrast
   (WCAG AA, 4.5:1 for body text). The accent is used for focus rings, the
   primary button fill and small labels; text on the accent uses
   `ThemeOnAccent`, which the `Theme` class picks (near-black or near-white)
   from the accent's luminance so a user-chosen accent never yields
   unreadable button text.

## Consequences

- Redesigned screens are theme-clean by construction. The old screens are
  not, and will look wrong in light mode until rebuilt; that is the visible
  debt, listed per screen in `TASKS.md`.
- The `DynamicResource` count in Rule 12's table becomes the leading measure
  of the redesign's progress.
- Third-party controls (the WPF ComboBox popup, ScrollBar) need restyling
  under both palettes; each is a small change on its own.
- The Dear ImGui debug overlay and the native render window are outside this
  ADR; they are not part of the shell's theme.

## Alternatives considered

- **Two full `ResourceDictionary` files swapped at runtime.** Works, but
  duplicates every style and invites drift between the two. Tokens plus a
  single style set keeps one source of truth.
- **Windows system accent only.** Rejected: the user asked for the colours to
  be theirs, and a console interface should not depend on the desktop's.
