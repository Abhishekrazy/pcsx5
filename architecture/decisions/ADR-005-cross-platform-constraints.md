# ADR-005: Cross-platform constraints — what to hold to from day one

- Status: Proposed
- Date: 2026-09-08
- Supersedes: None
- Superseded By: None

## Context

The question asked on 2026-09-08: *if the port starts today, what has to be kept
in mind so the emulator runs on all target platforms, with similar (not
identical) theming, and the backend behaving the same everywhere?*

This ADR is the answer as a set of standing constraints. It does not schedule
the port. It exists so that work done **before** the port does not make the port
harder, which is the cheapest possible contribution to it.

Two documents already bear on this and are not repeated here:
`docs/audits/AUDIT-2026-09-07-cross-platform-feasibility.md` (where the Windows
coupling lives, and the ordering of the core port) and ADR-003 (the token
palette the shell theming is built on).

## Current Evidence

Measured on 2026-09-08, not estimated:

| Coupling | Count | Label |
|---|---|---|
| Win32 API call sites in `src/**/*.cpp` | 127 | VERIFIED (grep) |
| `__try` blocks (structured exception handling) | 40 | VERIFIED (grep) |
| Public headers including `<windows.h>` | 7 | VERIFIED (grep) |
| Hand-written assembly using the **Windows** x64 ABI | `src/hle/dispatcher.asm` | VERIFIED |
| Guest TLS addressed at the hard-coded TEB offset | `0x1480` | VERIFIED |
| GPU backend | Vulkan, no D3D in the core | VERIFIED |
| Shell code-behind / XAML | 6,773 / 2,405 lines, no MVVM | VERIFIED (wc) |

Guest execution is **direct**: the PS4/PS5 CPU is x86-64 and so is the host, so
there is no CPU translation layer (`src/cpu/cpu.h`). The same is true of both
reference emulators examined, Kyty and SharpEmu.

## Decision

### 1. Platform reach is decided by the guest CPU, not by the UI

| Target | Reachable | Why |
|---|---|---|
| Windows x86-64 | Yes, today | — |
| Linux x86-64 | Yes, multi-week core port | POSIX equivalents exist for every coupling |
| macOS Intel | Yes, larger, with one risk | no public API sets the fs base for a user thread; guest TLS may be a `HARD BOUNDARY` there — `UNKNOWN` until tested |
| macOS Apple Silicon | **No** | ARM host, x86-64 guest, no recompiler |
| Android | **No** | same reason; see the Android entry in `TASKS.md` |

Do not plan a release around a target in the bottom two rows. Adding them means
specifying a guest recompiler as its own multi-quarter project first.

### 2. Nothing new may deepen the Windows coupling

These are the rules to hold from today, whether or not the port has started.
Each is cheap now and expensive later:

1. **No new `<windows.h>` in a public header.** Platform types go behind a
   single platform header. The existing 7 are debt to remove, not a precedent.
2. **No new `__try`.** Error handling that must survive a fault goes through the
   memory subsystem's guarded primitives, which already exist and are already
   the rule (Rule 05).
3. **No new direct Win32 call in a core subsystem.** OS services are reached
   through an adapter. Today the adapter is implicit; the port makes it
   explicit. Writing new code as if it existed costs nothing.
4. **The guest register context is PCSX5-owned.** Win32 `CONTEXT` is currently
   the ABI between the fault handler and the kernel. Any new code must not
   widen that; the port replaces it with a PCSX5 struct.
5. **Headless must keep working.** Already Rule 11. It is also the only way the
   core can be tested on a platform before its window and input layers exist.
6. **Per-target CMake links.** The global `link_libraries()` at the root is
   already recorded as a defect; it hard-links `hid`/`setupapi` into every
   target including tests. New targets link what they need, guarded by
   `if(WIN32)`.

### 3. Portability details that are cheap now and painful later

- **Paths.** No drive letters, no backslash separators, no assumption that the
  filesystem is case-insensitive. Linux will surface every `Assets/` vs
  `assets/` mismatch at once.
- **User directories.** Config, save data and caches must resolve through one
  function, not `./pcsx5_config` literals. Linux expects XDG paths and macOS
  expects `~/Library`; both differ from "next to the exe".
- **Line endings and text encoding.** Already a source of noise in this repo.
- **Threading.** Guest threads map 1:1 onto host threads; keep it that way. The
  TEB retargeting is the part that needs a POSIX equivalent, not the model.
- **No new platform-only dependency without an abstraction.** WASAPI, XInput
  and DualSenseWindows are each fine behind an interface and fatal in front of
  one.

### 4. Theming: same tokens, different backdrop

ADR-003 already put every colour behind named tokens referenced with
`DynamicResource`. That decision is what makes similar-but-not-identical theming
possible, because Avalonia supports the same pattern.

- **The palette is shared; the platform effects are not.** Mica and acrylic are
  Windows-only. Anything of that kind is a capability check with a defined
  fallback (a flat `ThemeGround`), never a hard requirement.
- **Native conventions are respected where they are load-bearing** — menu
  placement, window controls, default font — and overridden nowhere else.
- **The system light/dark preference is read per platform**, feeding the same
  `Theme` class. Only the detection differs.
- Accept that spacing and font metrics will not match pixel for pixel. Aim for
  the same identity, not the same screenshot.

### 5. "The backend runs the same" means one core and one test suite

- **One core, compiled everywhere.** No platform forks of a subsystem; only
  adapters differ. A `#ifdef` inside emulator logic is the failure mode.
- **The same configuration schema**, so a config file is portable even when the
  directory it lives in is not.
- **The same tests.** `ctest` must pass on every platform, and the run harness
  (`tools/game_runner/session.py`) must classify runs identically. A title that
  is `progressing` on Windows and `frozen` on Linux is a defect, not a platform
  difference.
- **Add the Linux CI job early — before the port is finished.** Even a job that
  only builds the subset that compiles will catch new coupling on the day it is
  introduced, which is the whole point. Retrofitting portability is expensive
  precisely because nothing measures it.

## Why

The audit already established that the core is the gate and a portable shell
ships nothing. What it did not say is what to do in the meantime. Most of the
cost of a port is not the port; it is the coupling added while the port was
"planned but not started". These constraints are the cheap half.

## Alternatives Considered

- **Port the shell first (Avalonia), then the core.** Defensible as de-risking,
  but must not be scheduled as "ship Linux/macOS" — it delivers no new platform.
- **Port the core first, keep WPF.** Delivers a headless Linux/macOS core, which
  is genuinely useful and testable, with no GUI.
- **Do nothing until the port is scheduled.** Rejected: the coupling grows. The
  shell code-behind has already grown from ~4,700 to 6,773 lines since Rule 12
  recorded it.

## Consequences

Some work gets slightly slower now — an adapter instead of a direct call. In
exchange the port stops being open-ended. None of these constraints require the
port to start, and all of them are useful on Windows alone.

## Compatibility Impact

None today. Every constraint is about new code.

## Runtime Impact

None intended. An adapter layer must not sit on a hot path without measurement;
the memory subsystem is the one to watch, since it is already the most
performance-sensitive code in the emulator.

## Testing Impact

The memory tests become characterization tests for the port (Rule 07) and must
pass unchanged across the platform abstraction. A Linux CI job is added early.

## Migration

The audit's ordering stands: header hygiene, then memory behind an abstraction,
then fault handling (a stopping condition — ask first), then threads and the
dispatcher, then periphery, then CMake.

Step 1 (removing `<windows.h>` from 7 public headers) is low risk, changes no
behaviour, and is the recommended starting point.

## Rollback / Containment

Each step keeps the Windows build green and is revertible on its own. Nothing
here requires a branch that cannot ship.

## Related Rules

Rule 05 (guest memory access), Rule 07 (testing integrity), Rule 10 (stopping
conditions), Rule 11 (UI shell boundary), Rule 12 (theming).

## Related ADRs

ADR-003 (shell theming — the token palette this depends on).
