# PCSX5 Clean Rebuild Operating Contract

## Authority

This file, [ARCHITECTURE.md](ARCHITECTURE.md), and [REBUILD_STATUS.md](REBUILD_STATUS.md) are the only project guidance sources. If another document or old source conflicts, these files win.

## Objective

Build a host-independent PS5 emulator core that supports Windows and Linux x64 first, then ARM64 hosts (Windows, Linux, macOS Apple Silicon, Android) through an x86-64-to-ARM64 recompiler.

The current implementation is a legacy reference, not the target architecture. Do not modify legacy emulator implementation files while Phase 0 is active unless the user explicitly requests a characterization fix.

## Working rules

1. Preserve behavior with tests or traces before redesigning a subsystem.
2. Mark claims `VERIFIED`, `INFERRED`, or `UNKNOWN`; do not invent hardware, ABI, OS, or compatibility behavior.
3. Keep `core` independent of OS, UI, graphics API, and platform SDK headers.
4. Platform code is leaf code behind narrow runtime contracts.
5. The interpreter is the correctness oracle. ARM64 JIT work starts only after its differential tests exist.
6. Vulkan is the initial graphics backend; Apple uses MoltenVK first. Native Metal requires measured evidence.
7. Never include proprietary firmware, keys, games, decrypted executables, or retail assets in the repository.
8. A target is supported only after its legal acceptance corpus passes; compilation alone is not support.
9. Do not add a dependency or make a destructive change without explicit user approval.
10. Keep each change small, buildable, and independently verifiable.

## Built-in engineering skills

| Skill | Use it for | Required output |
|---|---|---|
| Reconnaissance | Entering an unfamiliar subsystem | Ownership, call sites, tests, and unknowns |
| Characterization | Preserving legacy behavior | Focused test or deterministic trace |
| Contract design | Creating a cross-platform boundary | Narrow API, invariants, and capability model |
| Implementation | Changing one bounded concern | Buildable implementation with no unrelated refactor |
| Verification | Closing a change | Focused result, acceptance-gate result, and remaining risk |
| Architecture decision | Making an irreversible or cross-platform choice | Short decision record in `ARCHITECTURE.md` or a linked section |

## Standard workflow

1. Inspect the current boundary and tests.
2. Write or extend a characterization test/trace.
3. Specify the new contract in the architecture document.
4. Implement one bounded change.
5. Run focused tests, then the applicable acceptance gate.
6. Update `REBUILD_STATUS.md` with evidence and the next boundary.
7. Commit completed, verified task boundaries regularly using descriptive Conventional Commit messages. Stage only reviewed task-related changes; leave unfinished or unrelated work untouched. Record remaining risks rather than calling characterization failures fixed. Do not push remotely unless the user requests it.

## Source layout target

```text
core/        host-independent emulation state and devices
execution/   interpreter, direct-x64 containment, IR, x64/ARM64 backends
runtime/     memory, faults, threads, files, input, audio, timing per host
graphics/    HAL, Vulkan, MoltenVK, optional Metal
frontend/    platform shells only
tests/       legal unit, conformance, replay, and regression corpus
tools/       trace, diagnostics, build, and packaging utilities
```
