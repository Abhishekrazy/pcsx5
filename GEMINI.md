# PCSX5 Gemini Engineering Constitution

## Status

Mandatory project-level AI governance.

This file is the entry point for Gemini. It is not a suggestion list. Applicable rules and skills are engineering controls.

## Project Identity

PCSX5 is a Windows x64 PlayStation 5 emulator.

The repository already contains working emulator behavior. The current engineering program is a controlled architectural rehabilitation, not a greenfield rewrite.

The AI must optimize for:

```text
Correctness
  >
Architectural integrity
  >
Reproducibility
  >
Observability
  >
Compatibility evidence
  >
Performance
  >
Feature velocity
```

## Permanent Engineering Principles

1. Evidence before implementation.
2. Design before significant implementation.
3. Reuse before create.
4. One owner per architectural responsibility.
5. One authoritative source of truth per contract.
6. Runtime behavior is more authoritative than comments.
7. Tests prove only the behavior they exercise.
8. Passing tests do not prove title compatibility.
9. Existing working behavior is a compatibility reference.
10. Unknown behavior remains UNKNOWN.
11. No silent architecture drift.
12. No silent scope expansion.
13. No speculative PS5 behavior.
14. No test weakening to obtain green status.
15. No broad rewrite when incremental migration is possible.
16. No optimization before measurement.
17. No dependency survives merely because it already exists.
18. No dependency is added merely because it is convenient.
19. No public ABI changes without explicit architecture review.
20. No destructive Git operation without explicit authorization.

## Language

Communicate with the project owner in English unless explicitly requested otherwise.

## Technology Baseline

Current baseline:

- C++20: emulator core
- C: low-level libraries where justified
- x64 MASM: guest dispatcher/ABI bridge only where justified
- C# / .NET 9 / WPF: desktop shell
- Vulkan 1.3 headers/backend
- GLFW 3.3.9 where still justified
- Dear ImGui 1.90.4 docking for developer/debug tooling
- Lua 5.4.7 for controlled scripting/init
- nlohmann/json
- LibAtrac9
- stb_image / stb_vorbis where isolated
- FFmpeg and Bink2 only behind media boundaries
- NAudio only where justified on the UI side
- Win32 / WASAPI / XInput / HID platform adapters
- CMake >= 3.20
- CTest
- clang guest-test toolchain
- Python developer/RE tooling
- PowerShell/bash tooling
- WiX packaging

C++20 remains the baseline.

A language-standard upgrade requires architecture review and an ADR.

## Target Dependency Direction

```text
WPF UI
   |
   | stable versioned native ABI
   v
PCSX5 Runtime
   |
   +--> CPU
   +--> Memory/MMU
   +--> Kernel
   +--> HLE
   +--> Loader
   +--> GPU
   +--> Audio
   +--> Input
   +--> Timing
   +--> Debug/Trace
   |
   +--> platform adapters
              |
              +--> Win32
              +--> Vulkan
              +--> WASAPI
              +--> HID/XInput
```

The core must not depend on WPF.

HLE must not depend directly on Vulkan implementation details.

Platform APIs must not leak through every subsystem.

Game-specific compatibility code must not become generic emulator architecture.

## Truth Model

Every material engineering claim uses:

```text
SPECIFIED
IMPLEMENTED
VERIFIED
OPERATIONALLY CONFIRMED
```

And every investigation may additionally use:

```text
OBSERVED
INFERRED
HYPOTHESIS
UNKNOWN
```

Never report IMPLEMENTED as VERIFIED.

Never report VERIFIED as OPERATIONALLY CONFIRMED without runtime evidence.

## Current Emulator Rule

The current emulator is a behavioral reference.

Before replacing working behavior:

```text
Inventory
  ↓
Characterize
  ↓
Test
  ↓
Define contract
  ↓
Introduce boundary
  ↓
Migrate
  ↓
Compare
  ↓
Remove obsolete path
```

## Mandatory Stop Conditions

STOP and report instead of guessing when:

- undocumented PS5 behavior is required
- multiple architecture choices are equally plausible
- an accepted ADR would be violated
- a public ABI would change
- a subsystem ownership boundary would change
- a test must be weakened
- a crash must be hidden
- compatibility would rely on an unexplained title-specific hack
- a dependency replacement is required
- a destructive migration is required
- a long-running task begins expanding beyond its approved workstream
- completion cannot be supported by evidence

## Completion Is Not "Tests Pass"

A task is complete only when applicable:

- implementation is complete
- focused tests pass
- broader tests pass
- runtime behavior is checked where relevant
- architecture invariants remain true
- Claims vs Reality is reconciled
- documentation/ADR state is correct
- Git state is reported
- remaining gaps are explicit

## Autonomous Work

Routine low-risk work may be performed autonomously.

Significant architecture changes require planning/review first.

Never use autonomy as permission to redefine the architecture.
