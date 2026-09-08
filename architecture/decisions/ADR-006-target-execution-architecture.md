# ADR-006: Target execution architecture — backends, host runtime, graphics HAL

- Status: Proposed
- Date: 2026-09-08
- Supersedes: None
- Superseded By: None

## Context

On 2026-09-08 the owner sketched a target architecture for a genuinely
host-independent PCSX5:

```
                 PCSX5
                   |
        Host-independent core
                   |
       +-----------+-----------+
       |           |           |
     x64 JIT    ARM64 JIT   Interpreter
       |           |           |
       +-----------+-----------+
                   |
              Host Runtime
                   |
       +-----------+------------+
       |           |            |
    Windows      Linux       Apple/Android
       |           |            |
       +-----------+------------+
                   |
              Graphics HAL
                   |
             +-----+-----+
             |           |
          Vulkan       Metal
             |           |
          GPU vendors   Apple
```

This ADR assesses that shape against what exists, records two proposed
amendments, and — more usefully — separates the parts that can be built now
from the part that cannot.

## Current Evidence

The shape is correct. It is essentially the structure of every mature emulator
whose guest architecture differs from its host (RPCS3, Dolphin, PCSX2).

What exists today, measured 2026-09-08:

| Layer in the diagram | State in PCSX5 |
|---|---|
| Host-independent core | Does not exist as a boundary — 127 Win32 call sites, 40 `__try`, 7 public headers including `<windows.h>` |
| x64 JIT | None. Guest code executes **directly**; there is no translation layer |
| ARM64 JIT | None |
| Interpreter | None as such. `src/cpu/amd_compat.cpp` (366 lines) is a software fallback for **AMD-only Zen 2 bit-field instructions**, i.e. a handful of opcodes the host may lack |
| Host Runtime | Implicit and Windows-only. `src/hle/dispatcher.asm` uses the **Windows** x64 calling convention |
| Graphics HAL | Not a boundary, but the backend is already Vulkan and there is no D3D in the core |
| Metal | None. No MoltenVK either |

One asset worth naming: `src/kernel/instr_decode.cpp` (623 lines) is a real
x86-64 **disassembler**, but it produces text for crash reports and the TLS
patcher and is called only from the fault handler. It is not an execution
decoder and does not shorten the JIT work meaningfully.

## Decision

### 1. Accept the shape, with direct execution kept as a fourth backend

The execution row should read:

```
   Direct execution | x64 JIT | ARM64 JIT | Interpreter
```

On an x86-64 host running an x86-64 guest, direct execution is free and cannot
be beaten by any JIT — the guest instructions *are* host instructions. An x64
JIT for an x86-64 guest buys control (traps, instrumentation, determinism,
page-accurate memory hooks), not speed, and costs a great deal of both.

So: direct execution stays the default on x86-64 hosts. The x64 JIT becomes
optional, justified only if the control it buys is needed. **The ARM64 JIT is
where the real value is**, because it is the only thing that unlocks a platform
PCSX5 cannot otherwise reach.

The interpreter should be specified as the **correctness reference** — slow,
simple, and the thing a JIT is differentially tested against — not as a shipping
execution mode. That is what makes a JIT trustworthy.

### 2. Reach Metal through MoltenVK first, not a hand-written backend

A second full GPU backend is one of the largest items in the diagram, and the
existing Vulkan backend is substantial: GCN to SPIR-V translation, PM4 command
walking, render targets, presentation.

MoltenVK translates Vulkan to Metal and is the route most projects take. The
Graphics HAL should still exist as a boundary — it is good design and it is what
makes a native backend possible later — but its Apple leg should be MoltenVK
until MoltenVK is demonstrated insufficient for something this emulator actually
needs. Writing a native Metal backend first would be speculative.

Note the ordering constraint: on Apple Silicon the graphics question is moot
until the ARM64 JIT exists, because no guest code can run at all. Metal work
before the JIT delivers nothing.

### 3. Build the diagram top-down; the JIT is last, not first

This is the practically important part. The middle of the diagram is useful
**now**, on Windows alone, and does not depend on any JIT:

1. **Host-independent core** — the boundary itself. This is exactly what
   ADR-005 prescribes and what the cross-platform audit sequences: header
   hygiene, memory behind an abstraction, fault handling, threads, periphery.
2. **Host Runtime** — the adapter layer the above produces. Windows and Linux
   legs first; both are reachable today.
3. **Graphics HAL** — extracting the boundary is cheap and improves the code
   even with a single backend behind it.
4. **Interpreter** — buildable and testable on Windows, with no ARM in sight,
   and immediately valuable as a differential reference.
5. **ARM64 JIT** — last, and only once the four above exist. Attempting it
   first means building it against a core that still assumes Win32 faults, TEB
   retargeting and a Windows-ABI assembly dispatcher.

Steps 1 through 3 make the Windows emulator better regardless of whether the
port ever completes. That is the argument for doing them now.

## Why

The diagram's value is that it names the boundaries. Its risk is that it reads
as one project when it is really five, of which four are ordinary engineering
and one — the guest CPU layer — is a multi-quarter subsystem that does not exist
in any form.

Splitting them means the useful work is not blocked behind the hard work.

## Alternatives Considered

- **x64 JIT first, as a stepping stone to ARM64.** Rejected as the primary
  justification: it is a large amount of work whose main output is
  infrastructure, while direct execution already outperforms it. Worth
  reconsidering only if trap/instrumentation control is independently needed.
- **Native Metal backend.** Deferred to MoltenVK, see above.
- **Skip the interpreter, write the JIT directly.** Rejected. Without a
  reference implementation there is nothing to differentially test a JIT
  against, and guest CPU bugs surface as inexplicable game misbehaviour.

## Consequences

Accepting this means accepting that **Apple Silicon and Android remain out of
reach until an ARM64 JIT is specified as its own project**, and that the
intervening work is judged on whether it improves the emulator on platforms it
already runs on. Every item in steps 1 to 4 passes that test.

## Compatibility Impact

None today; this is a target architecture, not a change.

## Runtime Impact

Direct execution remains the default on x86-64, so no regression. A HAL boundary
must not be added on a hot path without measurement.

## Testing Impact

The interpreter, once it exists, becomes the differential reference for any JIT.
The existing memory tests become characterization tests for the host runtime
abstraction (Rule 07).

## Migration

Follow ADR-005's ordering. This ADR adds only that the interpreter precedes any
JIT, and that Metal is reached through MoltenVK.

## Rollback / Containment

Each of steps 1 to 4 is independently revertible and independently useful.

## Related Rules

Rule 05, Rule 07, Rule 09 (hard boundary), Rule 10 (stopping conditions).

## Related ADRs

ADR-005 (cross-platform constraints — the standing rules this builds on).
