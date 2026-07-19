# PCSX5 Development Roadmap

A phased plan for building a working PS5 emulator, grounded in the current state of this repository and lessons from PCSX2, RPCS3, shadPS4, Kyty, and RPCSX.

**Reality check:** No public emulator runs a commercial PS5 game today. PCSX5 already has the right core architecture (native x86-64 execution + HLE, the same model shadPS4 proved out). The GPU/shader-recompiler is the long pole — historically 50%+ of total effort in this class of emulator. Measure progress in phases, not games.

---

## Current Status Snapshot

| Subsystem | State | Notes |
|---|---|---|
| Loader (ELF/SELF) | ✅ Solid | `src/loader/elf.cpp` — ELF64, PT_SCE segments, RELA/PLT relocs, TLS, fPKG SELF. No retail decryption. |
| Kernel | 🟡 Partial | `src/kernel/` — ~30 syscalls, threads, TLS, fd table, VEH + INT3 syscall patching. |
| HLE modules | 🟡 Thin | `src/hle/` — libkernel has 122 symbols (~half return-0 stubs); pad/videoout/agc are stubs. |
| CPU layer | ✅ Done | `src/cpu/cpu.cpp` — `CpuCore` owns guest thread state; `src/kernel/thread.cpp` is a shim; pthread routes through it. |
| GPU | 🔴 Skeleton | `src/gpu/vulkan_backend.cpp` — window + Vulkan init only, no PM4 emulation. |
| Audio | 🟡 Preview only | ATRAC9 player (`src/ui/snd_player.cpp`); no libSceAudioOut HLE. |
| Input | 🟡 Basic | XInput/keyboard → PadButtonState in gpu backend; WPF DualSense HID reader. |
| UI | ✅ Working | WPF .NET 9 frontend (`src/ui_csharp/`) + CLI. |
| Tests | ✅ Good base | ~20 CTest targets; freestanding guest ELF smoke tests. |
| Compat infra | 🟡 Seeded | `src/compat/` DB with 6-status taxonomy; `compat/titles/` seeded with 5 titles; 1860 tracker issues created (all `status-nothing`). |

Legend: ✅ working · 🟡 partial · 🔴 not started

---

## Phase 0 — Foundations & Tooling

*Goal: unify the architecture and make progress measurable before adding features.*

- [x] Implement `src/cpu/cpu.cpp` — concrete `CpuCore` behind the API in `cpu.h`; single owner of guest thread state.
- [x] Consolidate the thread model: merge responsibilities currently split across `src/cpu/cpu.h`, `src/kernel/thread.cpp`, and `src/hle/libkernel.cpp` (pthread) into one documented flow.
- [x] Structured "unimplemented NID" logging: every stub logs module + NID + resolved name + hit count, exportable as JSON for compat reports (extend `src/hle/hle.cpp` import stats).
- [x] Seed `compat/titles/` with initial per-title JSONs for the games in `Games/`.
- [x] Populate `wiki/` with: architecture overview, how-to-add-HLE-symbols, how-to-add-syscalls, debugging guide (VEH, crash dumps).
- [x] CI workflow (GitHub Actions): build with MSVC + run full CTest suite on PRs.
- [x] Define compatibility status taxonomy (Nothing / Boot / Intro / Menus / Ingame / Playable) matching the tracker repo labels.

## Phase 1 — Loader & Module System ✅ (complete)

*Goal: load anything a dumped game actually contains, not just prepared fPKG ELFs.*

**Status (2026-07-19): complete — full build green, 24/24 CTest passing.** Recursive PRX auto-load and ModuleGraph→Kernel wiring landed (see items below); NID DB seeded. One follow-up carried forward (marked "carried"): real PKG/PFS fixtures.

- [x] PKG container extraction (Prospero PKG format; entry table, keys for fake-signed packages) — `src/loader/pkg.h/.cpp`; fPKG entry decryption via scene passcode; retail NPDRM detected and skipped. CLI: `pcsx5 --extract-pkg <file.pkg> <outdir>`.
- [x] PFS image parsing/mounting (read-only first) — games ship as PFS inside PKG. Parsing + extraction done (`src/loader/pfs.h/.cpp`); no guest-visible mount yet.
- [x] `param.json` parser (title id, version, category) feeding the compat DB and UI — PS5 ships `sce_sys/param.json`, not param.sfo; parser in `src/loader/param_json.h/.cpp`. (PS4-format `param.sfo` entries inside PKGs are only extracted raw by name-mapping; no SFO parser.)
- [x] PRX module loading from disk — recursive auto-load done in `src/kernel/kernel.cpp`: DT_NEEDED entries that resolve to on-disk PRX/SPRX files (via `ModuleResolver`) are mapped dependency-first (dedupe registry, cycle guard, depth cap 32) and linked against real exports; per-module failures fall back to HLE without aborting boot. PIE base relocation in `src/loader/elf.cpp` lets multiple modules coexist in the guest window.
- [x] Expand NID → name database — seeded `assets/nid_db.txt` with 706 verified entries (714 total) extracted from SharpEmu's `SysAbiExport` attributes (`sharpemu_clone/src/SharpEmu.Libs/**`), each validated via the PS5 name→NID hash; 8 known-bad mislabeled pairs excluded.
- [x] Module dependency graph + load-order resolution — `ModuleGraph` (`src/loader/module_graph.h/.cpp`) wired into the Kernel load flow: edges recorded during PRX discovery drive the dependency-first link order; cycles and HLE-served missing deps are logged via `CycleReport`.
- [x] Retail SELF decryption — **blocked/out of scope** until console key dumps are user-supplied; requirement and plugin point (`ExtractInnerElf` in `src/loader/elf.cpp`) documented in `wiki/self-decryption.md`.
- [ ] Tests: corpus of real (fake-signed) PKG/PFS fixtures (carried — synthetic fixture builders exist in `tests/pkg_tests.cpp` and `tests/pfs_tests.cpp`; real dumped fixtures not yet used).

## Phase 2 — Kernel Maturity

*Goal: `eboot.bin` of a real game starts executing and survives its early init.*

- [ ] Audit and complete the syscall table in `src/kernel/syscalls.cpp` against known Orbis/Prospero syscall numbers; stub-unknown-with-log policy.
- [ ] Replace return-0 pthread stubs in `src/hle/libkernel.cpp` with real implementations: mutex, condvar, rwlock, sema, event flag, once, tls keys.
- [ ] Equeue/event queues (`sceKernelCreateEqueue`, wait/poll) — heavily used by system modules.
- [ ] Memory: flexible mappings, direct/GPU-visible memory pools, `sceKernelReserveVirtualRange`, mprotect semantics — use VirtualAlloc placeholder regions (extend `src/memory/memory.cpp`).
- [ ] Signal/exception translation: map guest FreeBSD signals ↔ Windows SEH through the VEH path in `src/kernel/kernel.cpp`.
- [ ] Clock/time accuracy (`sceKernelGetProcessTime`, rtc) — games depend on monotonic timing early.
- [ ] Tests: extend `guest_syscall_smoke` guest ELFs to cover pthread + equeue + mmap scenarios.

## Phase 3 — Core HLE Modules

*Goal: the stub layer every game touches, in priority order. Workflow: stub aggressively, log unimplemented, implement what target games actually call (shadPS4 model).*

- [ ] libSceSysmodule (module load/unload routing to the loader).
- [ ] libSceLibcInternal (malloc family, stdio, string, setjmp) — decide HLE vs load-real-PRX per function.
- [ ] libSceUserService (user profiles, login state — mostly canned responses).
- [ ] libSceSystemService (param queries, status events).
- [ ] libSceNpManager / libSceNpCommon — offline stubs returning "not signed in".
- [ ] libSceSaveData (dialog-free direct API first).
- [ ] libSceVideoOut real flip/queue model (feeds Phase 5 presentation).
- [ ] Auto-generate stub registration from the NID database so every known export at least logs-and-returns.

## Phase 4 — First Game Bring-Up

*Goal: one simple commercial title reaches menus.*

- [ ] Pick 1–3 target titles from `Games/` (prefer smallest/simplest; record pick + rationale in compat DB).
- [ ] Drive boot to first unimplemented blocker; fix; repeat — log every iteration as a compat report (`src/reports/`).
- [ ] Real filesystem view for games: mount game PFS + app0/savedata path translation.
- [ ] Pad input end-to-end: DualSense/XInput → libScePad state → game reads it.
- [ ] Per-title config overrides exercised in anger (`src/config/`).
- [ ] Milestone: target title renders menu or reaches "needs GPU" wall — documented in compat tracker.

## Phase 5 — GPU (the long pole)

*Goal: translate GNM/PM4 to Vulkan. Expect this to be the largest single investment.*

- [ ] PM4 command-buffer parser: packet dispatch loop, register writes, draw/dispatch packets (`src/gpu/` new `pm4.*`).
- [ ] Clear + present path: render-target clear and swapchain blit — first visible output.
- [ ] Resource translation: guest GPU memory → Vulkan buffers/images (builds on Phase 2 direct memory).
- [ ] Shader recompiler: RDNA 2 ISA → SPIR-V (new `src/gpu/shader/` — this alone is a multi-month sub-project; study shadPS4's `shader_recompiler`).
- [ ] Pipeline state → Vulkan pipelines; descriptor translation; samplers.
- [ ] Texture formats/conversion table; tiling/detiling.
- [ ] Compute queue support.
- [ ] Shader cache (disk) + async pipeline compilation.
- [ ] Validation strategy: capture/replay tool for PM4 streams + golden-image tests.

## Phase 6 — Audio & Input

- [ ] libSceAudioOut HLE: ring-buffer output ports → host backend (XAudio2/SDL); mixing, volume, format conversion.
- [ ] Integrate ATRAC9 decode (LibAtrac9 already vendored) as an HLE audio codec path, not just the UI preview player.
- [ ] libScePad completeness: touchpad, motion sensor, headset endpoints.
- [ ] DualSense haptics/adaptive triggers via HID output (UI already has the HID reader).
- [ ] Tests: audio ring-buffer unit tests; pad state machine tests.

## Phase 7 — System Services

- [ ] Savedata write support with per-title encryption keys where required.
- [ ] PFS write support (savedata images).
- [ ] Trophy stubs → basic unlock event logging.
- [ ] Keystone parsing/validation stubs.
- [ ] Multi-user profile model in config.

## Phase 8 — Performance & Compatibility Scaling

- [ ] Memory fast paths: page-fault-free direct mappings, large-page support.
- [ ] Shader cache sharing + precompiled pipeline databases per title.
- [ ] Frame pacing / vsync / uncapped modes.
- [ ] Automated compat pipeline: run titles → generate reports → update `compat/titles/` → sync statuses to the compatibility tracker repo (extend `deploy_compatibility_repo.py`).
- [ ] Regression gates: `src/reports/` regression verdicts enforced in CI for target titles.
- [ ] Performance profiling tooling (frame graph, syscall heatmaps) in diagnostics.

---

## Reference Architectures (what we borrow from whom)

| Project | Lesson applied in PCSX5 |
|---|---|
| **PCSX2** | Mature per-game config/compat taxonomy; plugin-style subsystem boundaries; long-lived project hygiene. |
| **RPCS3** | Correctness-first HLE; heavy investment in logging/tooling and a public compat DB before per-game fixes; LLVM-grade shader/toolchain ambition. |
| **shadPS4** | Core architecture: native x86 guest execution + syscall trapping + NID-resolved HLE modules + PM4→Vulkan + shader recompiler. Closest template — study its linker, TLS, and `shader_recompiler`. |
| **Kyty** | PS4+PS5 dual-target scoping; Lua-driven subsystem init (already adopted in `src/lua/`). |
| **RPCSX** | RPCS3 veterans' PS4/PS5 path; validates the HLE-of-libkernel approach and slow, kernel-first bring-up. |

Sources: [shadPS4](https://github.com/shadps4-emu/shadPS4), [RPCS3](https://rpcs3.net), [RPCSX](https://github.com/RPCSX/rpcsx), [Kyty](https://github.com/InoriRus/Kyty), [Emulation General Wiki — shadPS4](https://emulation.gametechwiki.com/index.php/ShadPS4).

## Guiding Principles

1. **Native execution, HLE everything else** — no dynarec; keep the VEH/syscall-patch model.
2. **Stub first, implement on demand** — log unimplemented NIDs; let target games set priorities.
3. **Tooling before hacks** — every phase ships tests and compat reports, not just code.
4. **One target title at a time** — breadth comes after depth.
5. **Legal model** — firmware modules and keys come from the user's own console; never ship them.
