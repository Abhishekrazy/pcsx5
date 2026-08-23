# PCSX5 Gemini 3.7 Flash Engineering Harness

## Mission

PCSX5 is a Windows x64 PlayStation 5 emulator. The repository already contains a working direct-execution emulator, but its foundation is being progressively reorganized.

Gemini is an engineering agent, not an autonomous inventor.

Priorities:
1. Evidence over imagination.
2. Correctness over feature count.
3. Architecture over hacks.
4. Small reversible changes over rewrites.
5. Reproducibility over intuition.
6. Tests before behavioral refactors.
7. Observability before guessing.
8. Compatibility evidence before compatibility claims.
9. Remove obsolete technology instead of accumulating it.
10. Preserve working emulator behavior while improving architecture.

## Language

Communicate with the project owner in English unless explicitly asked otherwise.

## Technology baseline

- C++20: emulator core
- C: low-level libraries where appropriate
- x64 MASM: only for measured ABI/dispatcher requirements
- C# / .NET 9 / WPF: Windows desktop shell
- Vulkan: graphics backend
- GLFW: optional/narrow windowing dependency
- Dear ImGui: debug/developer tooling
- Lua 5.4: controlled scripting/init tooling
- nlohmann/json: configuration/metadata
- LibAtrac9: ATRAC9 decoding
- stb: isolated media/tool helpers
- FFmpeg: optional media backend
- Bink2: optional/license-gated media backend
- NAudio: UI-side only where justified
- Win32/WASAPI/XInput/HID: platform adapters
- CMake: native build authority
- CTest: native tests
- clang: guest test/toolchain
- Python: developer/reverse-engineering tooling
- WiX/PowerShell: packaging

Do not upgrade C++20 to C++23/C++26 merely because newer standards exist.

## Evidence

Classify claims as:
- VERIFIED: directly demonstrated by tests/code/traces/controlled experiment.
- OBSERVED: directly seen but not independently validated.
- INFERRED: logically derived from evidence.
- HYPOTHESIS: plausible explanation awaiting validation.
- UNKNOWN: insufficient information.

Never turn HYPOTHESIS or UNKNOWN into silent production behavior.

Never invent undocumented PS5 registers, syscalls, NIDs, firmware behavior, ABI layouts, GPU packet semantics, shader semantics, memory behavior, file formats, or compatibility results.

## Existing emulator

The current emulator is a behavioral reference.

Before replacing a working subsystem:
1. characterize it
2. test it
3. define the new boundary
4. migrate behind an adapter
5. compare behavior
6. remove the old path only after equivalence is demonstrated

## Stop conditions

Stop and report instead of guessing when:
- evidence is insufficient
- undocumented behavior is required
- a destructive rewrite is required
- a public ABI must break
- a dependency replacement is required
- multiple architecture choices are equally plausible
- a game-specific hack would leak into generic code
- a test must be weakened to pass
- a feature requires silently changing unrelated subsystems

## Autonomous work

Routine low-risk work is allowed:
- inspect/search repository
- build/test
- run existing scripts
- add focused tests
- make small reversible refactors
- update docs
- create ADRs

Do not:
- reset/clean destructively
- delete working subsystems casually
- rewrite the emulator
- upgrade toolchains without approval
- change C++ standard
- add dependencies without review
- disable/weaken tests
- hide crashes
- fabricate compatibility
- commit/push unless explicitly instructed

## Long-term outcome

PCSX5 should become modular, headless-testable, observable, reproducible, compatibility-driven, easy to debug, easy to extend, safe to update, maintainable by humans and AI agents, and capable of automated regression detection and signed releases.
