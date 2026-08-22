# PCSX5 Claude Code Guardrails

## Mission

PCSX5 is a Windows x64 PlayStation 5 emulator. The repository already contains a working, but architecturally tangled, emulator. The goal is to progressively correct the foundation without destroying working behavior.

Claude must optimize for:

1. Correctness over cleverness.
2. Evidence over imagination.
3. Small reversible changes over rewrites.
4. Explicit subsystem ownership.
5. Deterministic builds and tests.
6. Debuggability and observability.
7. Compatibility work backed by reproducible evidence.
8. Removing obsolete technology rather than accumulating it.

## Non-negotiable behavior

- Do not invent undocumented PS5 hardware behavior and present it as fact.
- Do not invent APIs, registers, instruction semantics, firmware behavior, file formats, ABI rules, or compatibility results.
- Mark uncertain behavior as `UNKNOWN`, `HYPOTHESIS`, or `NEEDS_EVIDENCE`.
- Never silently change emulator semantics during a refactor.
- Never replace a working subsystem wholesale unless the user explicitly approves it.
- Prefer characterization tests around existing behavior before moving code.
- Preserve a buildable repository at every meaningful step.
- Do not add a dependency when an existing dependency or small internal abstraction is sufficient.
- Do not add a technology because it is fashionable.
- Do not create parallel implementations without a migration/removal plan.
- Do not create "temporary" abstractions without an owner, purpose, and removal condition.
- Do not hide architectural problems with giant utility classes, global state, singletons, or catch-all managers.
- Do not put emulator core logic in the WPF UI.
- Do not make the core depend on WPF, .NET, GLFW, ImGui, or Windows UI concerns.
- Do not make game-specific hacks part of generic CPU/GPU/kernel code. Isolate them behind compatibility profiles or explicit workarounds.
- Do not download prebuilt executables into the source tree as a substitute for source changes.
- Do not commit generated binaries, build directories, logs, crash dumps, caches, or local secrets unless explicitly required.
- Do not change compiler/toolchain versions casually.
- Do not change public interfaces without documenting the compatibility impact.
- Do not use unsafe undefined behavior in the emulator core to gain a micro-optimization without measurement and a fallback strategy.

## Architectural direction

The target architecture is:

```text
pcsx5.exe / WPF UI
        |
        | stable C ABI / PInvoke boundary
        v
pcsx5_core.dll
        |
        +-- Emulator Runtime / Session
        +-- Guest CPU
        +-- Memory / MMU
        +-- Kernel / Syscalls
        +-- HLE Services
        +-- Loader / ELF / Modules
        +-- GPU / GCN-RDNA2 translation
        +-- Audio
        +-- Input
        +-- Timing / Synchronization
        +-- Debug / Tracing
        |
        +-- Platform adapters
        |     +-- Win32
        |     +-- Vulkan
        |     +-- WASAPI
        |     +-- XInput/HID/DualSense
        |
        +-- Third-party libraries
```

The core must be usable headlessly for tests.

## Refactoring protocol

Before changing an unfamiliar subsystem:

1. Inspect the current implementation.
2. Identify ownership and dependencies.
3. Find all call sites.
4. Find tests, logs, save-state behavior, and compatibility evidence.
5. Add or improve characterization tests.
6. Write the intended boundary.
7. Make one small migration.
8. Build.
9. Run focused tests.
10. Run regression tests.
11. Record architectural changes in an ADR when the decision is durable.

If the existing behavior is unknown, do not guess. Instrument it.

## Change budget

For a normal task, prefer:

- 1 architectural concern
- 1 subsystem or narrow slice
- 1 buildable change
- 1 focused test addition/update

If a task would touch many unrelated subsystems, stop and propose a sequence of smaller changes.

## Technology policy

Current preferred baseline:

- C++20 for emulator core.
- C for LibAtrac9 and other genuinely C-oriented low-level libraries.
- MASM only where a measured ABI/assembly requirement exists. It is not a default optimization tool.
- C# / .NET for the Windows desktop UI.
- WPF is acceptable for the desktop shell. Keep it outside the emulator core.
- Vulkan for the GPU backend.
- GLFW only for components that actually need its cross-platform window/input facilities. It should not become a hidden dependency of the WPF UI.
- Dear ImGui for developer/debug tooling, preferably hosted by a dedicated debug frontend or controlled core debug layer.
- Lua for intentionally exposed scripting, not for core emulator behavior.
- nlohmann/json for human-editable configuration and localization metadata where appropriate.
- CMake as the authoritative native build system.
- CTest for native tests.
- clang/LLD or a controlled clang toolchain for guest test ELF generation where needed.
- Python for developer tooling and reverse-engineering utilities, not runtime emulator logic.
- .NET SDK for UI build/publish.
- WiX only if installer requirements justify it.
- FFmpeg only behind a narrow media abstraction and with licensing/distribution requirements documented.
- Bink2 only if the project has the required redistribution/licensing rights.
- NAudio only on the UI/platform side where it is genuinely needed. Do not duplicate native audio paths without a reason.

Potential technology reductions to evaluate:

1. MASM: remove if the dispatcher can be expressed safely in portable C++/intrinsics without measurable regression.
2. GLFW: remove from the main WPF path if WPF owns the window and input.
3. ImGui: keep debug-only and optional if it does not belong in release/runtime paths.
4. NAudio: remove if WASAPI/native audio is sufficient and a duplicate audio path creates complexity.
5. Bink2: isolate and make optional because licensing and distribution constraints may be external.
6. FFmpeg: keep optional and isolated. Do not make the emulator core depend on it.
7. Lua: keep only if scripts have a stable, documented purpose.
8. Any third-party library must have an owner, version pin, license note, update procedure, and reason for existence.

## Definition of done

A task is not done merely because it compiles.

For code changes, "done" normally means:

- builds with the supported toolchain
- relevant tests pass
- no new warnings that are knowingly ignored
- behavior is preserved or intentionally changed
- logs/debugging remain useful
- ownership is clear
- no new dependency was introduced without justification
- docs are updated if architecture or public behavior changed

## When blocked

If evidence is insufficient, say exactly what is unknown and what evidence would resolve it. Do not fill the gap with an invented implementation.
