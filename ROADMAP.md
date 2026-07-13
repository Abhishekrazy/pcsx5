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
- [ ] Test targets, compatibility database, and reproducible regression suite.
- [ ] Typed and verified HLE ABI contracts.
- [ ] PS5-style GPU command processing and rendering.

## Phase 0 — Engineering foundation (in progress)

### Objectives

- [~] Add CMake/CTest targets for loader, memory, HLE, and guest smoke tests.
  - [x] Loader validation target: truncated headers, invalid ELF magic, and
    unsupported machine architecture.
- [ ] Create a test corpus of valid, malformed, PIE, fixed-address, relocation,
  TLS, thread, and file-I/O ELF fixtures. Fixtures must be self-authored or
  otherwise redistributable.
- [ ] Define structured logging, a crash-report bundle, and import-call tracing.
- [ ] Add a versioned configuration service with global and per-title files.
- [ ] Publish compatibility-report and regression-report templates.
- [ ] Define supported-host requirements and a contributor build guide.

### Current sprint: guest execution correctness

1. Wire the existing freestanding guest ELF into an automated smoke test.
2. [x] Add loader validation tests for truncated headers, invalid ELF identifiers, unsupported architecture, program-header bounds, and segment bounds.
3. [~] Replace the repeated TLS-read trap with a documented guest TLS/thread-context
   model and add a dedicated TLS test guest.
   - [x] Bounded TLS address translation and automated CTest coverage.
   - [ ] Guest ELF fixture exercising FS-relative reads and writes.
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
