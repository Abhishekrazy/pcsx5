# PCSX5 Roadmap

> **Status:** Early prototype / foundation phase  
> **Last updated:** 2026-07-13

PCSX5 is intended to become a legal, user-friendly PlayStation 5 compatibility
project for Windows x64. It must only operate on lawfully obtained, user-dumped
software and must not contain proprietary Sony firmware, keys, or DRM-bypass
code.

## Principles

- Correctness and diagnostics come before performance enhancements.
- Vulkan is the primary rendering API. OpenGL is a secondary compatibility and
  debugging renderer, introduced only after the Vulkan path is correct.
- Every compatibility fix needs a reproducible test, trace, or documented title
  regression.
- Settings have safe global defaults and narrowly scoped per-game overrides.
- Unknown imports must be visible and actionable; they must not silently pretend
  to be correct.

## Compatibility milestones

| Label | Meaning |
| --- | --- |
| Loads | Executable validation and mapping finish without a fatal error. |
| Boots | Reaches the application entry point. |
| Intro | Renders or plays initial application content. |
| In-game | Reaches interactive gameplay. |
| Playable | Stable enough for sustained play with known limitations documented. |

## Current baseline

- [x] CMake/MSVC Windows build and modular source layout.
- [x] ELF64/x86-64 header and load-segment mapping prototype.
- [x] Basic HLE registry, kernel exception handling, input/video/audio stubs, and
  Vulkan presenter prototype.
- [x] A freestanding guest ELF smoke-test source exists in `tests/test_elf/`.
- [~] Guest TLS has a bounded, testable thread-context model. Guest-code TLS
  execution still uses the exception bridge and needs a dedicated fixture.
- [x] Test targets, compatibility database, and reproducible regression suite.
- [ ] Typed and verified HLE ABI contracts.
- [ ] PS5-style GPU command processing and rendering.

## Phase 0 — Engineering foundation (in progress)

### Objectives

- [x] Add CMake/CTest targets for loader, memory, HLE, and guest smoke tests.
  - [x] Loader validation target: truncated headers, invalid ELF magic, and
    unsupported machine architecture.
  - [x] Memory validation target: Map / Unmap / Protect / Read / Write /
    Buffer / Translate / Reserve / Commit.
  - [x] HLE subsystem test target: import reporting, strict mode,
    NID-variant resolution, and stub accounting.
  - [x] Guest smoke test target: freestanding syscall ELF and FS-relative
    TLS ELF driven through the full emulator with `--strict-imports`.
- [x] Create a test corpus of valid, malformed, PIE, fixed-address,
  relocation, TLS, thread, and file-I/O ELF fixtures.  Fixtures are
  self-authored or otherwise redistributable.
  - [x] Hand-crafted positive corpus (`tests/loader_corpus.cpp`): valid
    PIE, fixed-address, multi-segment, BSS, PT_DYNAMIC with
    DT_STRTAB / DT_SYMTAB / DT_RELA.  87 checks.
  - [x] Hand-crafted negative corpus (`tests/loader_validation.cpp`):
    truncated headers, bad magic, unsupported architecture, program-header
    bounds, segment bounds.
  - [x] Freestanding guest fixtures (`tests/test_elf/`): syscall smoke
    test, FS-relative TLS test, and a build script for PIE / fixed /
    relocation variants when clang is on PATH.
- [x] Define structured logging, a crash-report bundle, and import-call tracing.
  - [x] Structured log records with timestamp, category, level, file/line/func,
    and a 1024-entry ring buffer for crash reports.
  - [x] JSON and ANSI console output, optional file mirror via `--log-file=`.
  - [x] 256-entry HLE import-call trace (module, NID, args, caller RIP) exposed
    via `HLE::GetImportTrace` and embedded in the crash bundle.
  - [x] SEH-based crash filter (`SetUnhandledExceptionFilter`) that captures a
    full register snapshot, recent log ring, and import trace, then writes a
    bundle (crash.json, registers.json, recent.log, import_trace.txt,
    system.txt, minidump.dmp) to `--crash-dir=`.
  - [x] `diagnostics_tests` CTest target with 26 checks covering the log ring,
    file mirror, import trace, and bundle layout.
- [x] Add a versioned configuration service with global and per-title files.
  - [x] Schema version stamped on every file (`schema_version: 1`) and a
    migration pass on load.  Unknown future versions are passed through with
    a warning.
  - [x] Sectioned config (`logging`, `crash`, `hle`, `graphics`, `audio`,
    `input`) with `Defaults()` for compile-time fall-back and JSON
    serialisation handled by a hand-rolled parser (no third-party dep).
  - [x] Global file at `<config_dir>/global.json` and per-title overrides at
    `<config_dir>/titles/<title_id>.json`; `EffectiveFor(title_id)` overlays
    per-title on top of global.
  - [x] CLI flags `--config-dir=` and `--title-id=` wire the service in
    `main.cpp` ahead of every other subsystem so logging / crash / HLE
    settings are applied at startup.
  - [x] `config_tests` CTest target with 51 checks covering defaults, full
    round-trip, missing-file auto-write, corrupt-file fall-back, per-title
    override layering, and forward-compat with newer schema versions.
- [ ] Publish compatibility-report and regression-report templates.
- [ ] Define supported-host requirements and a contributor build guide.

### Current sprint: guest execution correctness

1. Wire the existing freestanding guest ELF into an automated smoke test.
2. [x] Add loader validation tests for truncated headers, invalid ELF identifiers, unsupported architecture, program-header bounds, and segment bounds.
3. [x] Replace the repeated TLS-read trap with a documented guest TLS/thread-context
   model and add a dedicated TLS test guest.
   - [x] Bounded TLS address translation and automated CTest coverage.
   - [x] Guest ELF fixture exercising FS-relative reads and writes.
4. Make HLE import resolution report module, NID, caller, call count, and
   argument ABI; fail loudly in test mode for unresolved imports.
5. Record the resulting status in the compatibility database.

**Exit criteria:** self-authored test ELFs can load, execute, use TLS, issue
supported calls, and exit deterministically; the test run is automated.

## Phase 1 — Loader, memory, process, and kernel

- [ ] Complete relevant decrypted-input ELF/SELF metadata parsing: dynamic data,
  relocations, imports/exports, TLS, and module dependencies.
- [ ] Implement guest virtual memory: reserve/map/unmap/protect/query, fault
  handling, page alignment, and correct error codes.
- [ ] Implement guest process and thread lifecycle, scheduler primitives,
  mutexes, semaphores, condition variables, timers, and events.
- [ ] Implement a sandboxed virtual filesystem, mount table, save-data paths,
  and metadata semantics.
- [ ] Provide a debugger: module view, guest memory view, HLE trace,
  breakpoints, and fault reports.

**Exit criteria:** test programs reliably use threads, TLS, synchronization, and
file I/O.

## Phase 2 — HLE system services

- [ ] Convert HLE stubs to typed, versioned module APIs with documented NIDs,
  layouts, return values, blocking behavior, callbacks, and lifetimes.
- [ ] Prioritize implementations with import-frequency reports: `libkernel`,
  system/user services, pad, audio out, video out, save data, networking, and
  common runtimes.
- [ ] Build direct HLE API tests and callback/threading tests.
- [ ] Maintain an unresolved-import queue per title and firmware/API version.

**Exit criteria:** selected legal test applications reach their main loop with
no automatic unknown-function success stubs.

## Phase 3 — Graphics (Vulkan first)

- [ ] Create GPU memory, resource, synchronization, command-buffer, descriptor,
  render-target, and presentation abstractions.
- [ ] Decode relevant graphics commands and translate shaders to host Vulkan
  pipelines.
- [ ] Add pipeline/shader caches, resource-state tracking, texture caching, GPU
  markers, validation support, and frame capture/replay.
- [ ] Implement format, depth/stencil, blending, compute, and presentation paths
  incrementally, using captured test frames.
- [ ] Define a renderer interface and add an OpenGL compatibility/debug backend
  after Vulkan is stable; it must share guest command decoding and shader
  translation inputs rather than duplicating emulation logic.

**Exit criteria:** graphics test applications render repeatable frames, followed
by menus and 3D scenes in a tracked target title.

## Phase 4 — Audio and input

- [ ] Model audio queues/timing, then provide WASAPI/XAudio2 output with
  resampling, channel mapping, drift correction, underrun telemetry, and
  Balanced/Low-latency/Stable profiles.
- [ ] Add SDL-based controller discovery and mapping: keyboard/mouse, XInput,
  DualSense, DualShock 4, generic controllers, hot-plugging, dead zones,
  motion, touchpad, and supported rumble/haptics.
- [ ] Ship controller setup, input testing, hotkeys, and per-game profiles.

**Exit criteria:** stable audio under load and controller mappings that persist
across restarts, with measurable latency.

## Phase 5 — Launcher and user experience

- [ ] Build a game library with metadata, artwork cache, play time, launch
  status, logs, saves, and compatibility badges.
- [ ] Add desktop and controller-first interfaces plus an in-game overlay.
- [ ] Provide global/per-game settings, hardware capability detection,
  recommended defaults, onboarding, error reports, and portable mode.
- [ ] Add patch/mod management through declarative, auditable per-title patches
  and a controlled plugin API—not an arbitrary process injector.

## Phase 6 — Enhancements and performance

- [ ] Add frame pacing, VSync/VRR, HDR where supported, resolution scaling,
  aspect-ratio controls, texture filtering, screenshots, and capture.
- [ ] Add an upscaler abstraction: native, bilinear, FSR 1, then temporal FSR
  only when accurate motion/depth data is available.
- [ ] Optimize using profiling data: pipeline cache behavior, background shader
  compilation, scheduler tuning, and per-game accuracy/performance profiles.

## Phase 7 — Compatibility operations

- [ ] Maintain a public compatibility database and title-specific issue tracker.
- [ ] Add automated boot/regression/performance testing and release criteria.
- [ ] Publish privacy-respecting diagnostic bundles and contributor procedures.

## Definition of done

A feature is done only when its behavior is documented, has tests or a captured
regression, exposes safe defaults in the UI where relevant, and does not create
new unresolved-import, crash, or frame-correctness regressions in the test set.
