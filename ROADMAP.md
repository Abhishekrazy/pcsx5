# PCSX5 Development Roadmap

**Status:** target architecture, not a schedule. Proposed 2026-09-08.
**Authoritative sources:** [ADR-005](architecture/decisions/ADR-005-cross-platform-constraints.md)
(standing constraints), [ADR-006](architecture/decisions/ADR-006-target-execution-architecture.md)
(this shape, assessed), `docs/audits/AUDIT-2026-09-07-cross-platform-feasibility.md`
(where the Windows coupling actually is).

## The shape we are building toward

```text
                 PCSX5
                   │
        Host-independent core
                   │
   ┌───────────┬───┴───┬───────────┐
   │           │       │           │
 Direct      x64 JIT ARM64 JIT  Interpreter
 execution  (optional)         (correctness
 (default,                       reference)
  x86-64)
   │           │       │           │
   └───────────┴───┬───┴───────────┘
                   │
              Host Runtime
        (memory · faults · threads
         · time · paths · periphery)
                   │
       ┌───────────┼────────────┐
       │           │            │
    Windows      Linux       Apple/Android
       │           │            │
       └───────────┼────────────┘
                   │
              Graphics HAL
                   │
             ┌─────┴─────┐
             │           │
          Vulkan       Metal
             │           │
          GPU vendors  MoltenVK first,
                       native later if
                       ever justified
```

Three amendments to the original sketch, each argued in ADR-006:

1. **Direct execution is a fourth backend, and the default.** The PS5 CPU is
   x86-64 and so is the host, so guest instructions *are* host instructions
   (`src/cpu/cpu.h`, VERIFIED). An x64 JIT for an x86-64 guest buys control —
   traps, instrumentation, determinism — not speed, so it is optional.
2. **The interpreter is the correctness reference, not a shipping mode.** It is
   what a JIT is differentially tested against; without it, guest CPU bugs
   surface as inexplicable game misbehaviour.
3. **Metal is reached through MoltenVK.** The HAL boundary still exists — that
   is what makes a native backend possible later — but writing a second full
   GPU backend before MoltenVK is shown insufficient would be speculative.

The desktop shell is *not* in this diagram on purpose. It is a consumer of the
core across the C ABI (Rule 11), and porting it delivers no new platform on its
own.

## Where we actually are

Measured 2026-09-08, not estimated:

| Layer | State |
|---|---|
| Host-independent core | Not a boundary. 127 Win32 call sites in `src/**/*.cpp`, 40 `__try` blocks, 7 public headers including `<windows.h>` |
| Direct execution | Works, and is the whole CPU story today |
| x64 JIT / ARM64 JIT / Interpreter | None. `src/cpu/amd_compat.cpp` (366 lines) is a software fallback for a handful of AMD Zen 2 bit-field opcodes, not an interpreter |
| Host Runtime | Implicit and Windows-only. `src/hle/dispatcher.asm` uses the **Windows** x64 calling convention; guest TLS is addressed at the hard-coded TEB offset `0x1480` |
| Graphics HAL | Not a boundary — but the backend is already Vulkan and there is no D3D in the core, which is the expensive half already done |
| Metal / MoltenVK | None |
| Shell | WPF, 6,773 lines of code-behind + 2,405 XAML, no MVVM |

One asset worth naming: `src/kernel/instr_decode.cpp` (623 lines) is a real
x86-64 disassembler, but it produces text for crash reports and the TLS patcher
and is called only from the fault handler. It does not shorten the JIT work.

## Platform reach is decided by the guest CPU, not by the UI

| Target | Reachable | Why |
|---|---|---|
| Windows x86-64 | Yes, today | — |
| Linux x86-64 | Yes, multi-week core port | POSIX equivalents exist for every coupling |
| macOS Intel | Yes, larger, one risk | No public API sets the fs base for a user thread; guest TLS may be a `HARD BOUNDARY` there — `UNKNOWN` until tested |
| macOS Apple Silicon | No, until the ARM64 JIT | ARM host, x86-64 guest, no recompiler |
| Android | No, until the ARM64 JIT | Same reason; recorded as a hard boundary in `TASKS.md` |

Do not plan a release around the bottom two rows.

## Build it top-down; the JIT is last

The middle of the diagram is useful **now, on Windows alone**, and depends on no
JIT. That is the argument for doing it first: steps 1–4 improve the emulator we
ship today, and each is independently revertible.

### Step 1 — Host-independent core (the boundary itself)

Ordering from the feasibility audit, cheapest and least risky first:

1. Header hygiene: remove `<windows.h>` from the 7 public headers, behind one
   platform header. Low risk, no behaviour change, **the recommended starting
   point**.
2. Memory behind an abstraction. The existing memory tests become the
   characterization tests for it (Rule 07) and must pass unchanged.
3. Fault handling — replacing 40 `__try` blocks and the Win32 `CONTEXT` ABI
   between the fault handler and the kernel with a PCSX5-owned register context.
   *This is a stopping condition (Rule 10): ask before starting it.*
4. Threads and `dispatcher.asm` — the guest 1:1 thread model stays; the TEB
   retargeting needs a POSIX equivalent, and the dispatcher needs a SysV path.
5. Periphery: WASAPI, XInput, DualSenseWindows each behind an interface.
6. CMake: remove the root `link_libraries()` that hard-links `hid`/`setupapi`
   into every target including tests; per-target links guarded by `if(WIN32)`.

### Step 2 — Host Runtime

The adapter layer step 1 produces, made explicit. Windows and Linux legs first;
both are reachable today. One core compiled everywhere — no platform forks of a
subsystem, only adapters. A `#ifdef` inside emulator logic is the failure mode.

### Step 3 — Graphics HAL

Extract the boundary with a single backend behind it. Cheap, improves the code
now, and is the precondition for MoltenVK later. Do not add a HAL indirection on
a hot path without measurement.

### Step 4 — Interpreter

Buildable and testable on Windows with no ARM in sight, and immediately valuable
as the differential reference that makes any future JIT trustworthy.

### Step 5 — ARM64 JIT

Last, and only once steps 1–4 exist, because attempting it earlier means
building it against a core that still assumes Win32 faults, TEB retargeting and
a Windows-ABI assembly dispatcher. This is a multi-quarter subsystem specified
as its own project — the one item here that is not ordinary engineering.

Metal work on Apple Silicon is moot until this exists: no guest code runs at all
before it.

### Step 6 — Shell portability (parallel track, ships nothing alone)

Avalonia is the chosen framework (ADR-005 §4, decided 2026-09-07). It runs in
parallel with the core work and must never be scheduled as "ship Linux/macOS":
the core is the gate. ADR-003's token palette referenced with `DynamicResource`
is what makes similar-but-not-identical theming possible. Mica and acrylic are
Windows-only capability checks with a flat `ThemeGround` fallback, not
requirements. Aim for the same identity, not the same screenshot.

## Standing constraints — hold these from today

Whether or not the port has started, each of these is cheap now and expensive
later (ADR-005 §2):

1. **No new `<windows.h>` in a public header.** The existing 7 are debt to
   remove, not a precedent.
2. **No new `__try`.** Faults go through the memory subsystem's guarded
   primitives, which already exist and are already the rule (Rule 05).
3. **No new direct Win32 call in a core subsystem.** Write as if the adapter
   already existed; it costs nothing.
4. **The guest register context is PCSX5-owned.** Do not widen the Win32
   `CONTEXT` ABI.
5. **Headless must keep working** (Rule 11). It is the only way to test the core
   on a platform before its window and input layers exist.
6. **Per-target CMake links**, guarded by `if(WIN32)`.
7. **No drive letters, no backslashes, no case-insensitive filesystem
   assumption.** Linux surfaces every `Assets/` vs `assets/` mismatch at once.
8. **User directories resolve through one function** — XDG on Linux,
   `~/Library` on macOS, never a `./pcsx5_config` literal.
9. **No new platform-only dependency without an abstraction.** WASAPI, XInput
   and DualSenseWindows are each fine behind an interface and fatal in front of
   one.

## What "the backend runs the same" means

One core, one configuration schema, one test suite. `ctest` passes on every
platform and `tools/game_runner/session.py` classifies runs identically: a title
that is `progressing` on Windows and `frozen` on Linux is a defect, not a
platform difference.

**Add the Linux CI job early — before the port is finished.** Even a job that
only builds the subset that compiles catches new coupling on the day it is
introduced. Retrofitting portability is expensive precisely because nothing
measures it.

## How this is tracked

Open work, with its evidence, lives in [TASKS.md](TASKS.md); finished work in
[docs/COMPLETED_TASKS.md](docs/COMPLETED_TASKS.md). This file states the target
and the ordering; it is not a progress tracker and is not updated per task.

Nothing above is a rewrite. "Ground up" here means *re-deriving the boundaries*,
not discarding a working emulator: no subsystem is replaced wholesale, every
step keeps the Windows build green, and each is revertible on its own.
