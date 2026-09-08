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
7. Follow the commit checkpoints below before moving to another task or handing off completed work.

## Commit checkpoints

- Continue automatically across completed tasks and phases; do not wait for another
  "go" at routine checkpoints. Stop only for missing authority/access/hardware or
  a material owner decision, and never turn an unverified gate into a completion
  claim. Preserve this rule when handing work across context boundaries.

- Commit after each completed, verified task, not only at the end of a phase. During longer tasks, commit independently buildable and tested increments when available.
- Before committing, review the diff, run the relevant checks, and update `REBUILD_STATUS.md` with evidence and remaining risks. Never label a failed or unverified gate complete merely to make a checkpoint.
- Use descriptive Conventional Commit messages on a `codex/` branch. Stage only reviewed task-related paths; leave unfinished, unrelated, generated and secret material out.
- Before switching tasks or finishing a turn with completed changes, create the checkpoint and report its hash. If a commit cannot safely be made, report why and what remains uncommitted.
- Do not push remotely or rewrite existing history unless the user requests it.

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
