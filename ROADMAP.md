# PCSX5 Development Roadmap

A phased plan for building a working PS5 emulator, grounded in the current state of this repository and lessons from PCSX2, RPCS3, shadPS4, Kyty, and RPCSX.

**Reality check:** No public emulator runs a commercial PS5 game today. PCSX5 already has the right core architecture (native x86-64 execution + HLE, the same model shadPS4 proved out). The GPU/shader-recompiler is the long pole — historically 50%+ of total effort in this class of emulator. Measure progress in phases, not games.

---

## Current Status Snapshot

| Subsystem | State | Notes |
|---|---|---|
| Loader (ELF/SELF) | ✅ Solid | `src/loader/elf.cpp` — ELF64, PT_SCE segments, RELA/PLT relocs, TLS, fPKG SELF. No retail decryption. |
| Kernel | ✅ Mature | `src/kernel/` — syscalls, threads, TLS, fd table, VEH + INT3 patching, demand-commit guest fault handler, unified guest clock; real pthread/equeue/sema/event-flag sync in `src/hle/libkernel_sync.cpp`. |
| HLE modules | ✅ Broad | `src/hle/` — savedata, user-service, system-service, NP, sysmodule, libc heap + printf engine, videoout flip model, AGC/PM4; 553 NID-DB stubs auto-registered. |
| CPU layer | ✅ Done | `src/cpu/cpu.cpp` — `CpuCore` owns guest thread state; `src/kernel/thread.cpp` is a shim; pthread routes through it. |
| GPU | 🟡 Rising | Real Vulkan backend (`src/gpu/vk_context.cpp`/`vk_present.cpp` — swapchain present proven pixel-correct, GDI fallback); PM4 DCB builders + submit walker with register shadow (`src/hle/libagc.cpp`); RDNA2 decoder + metadata + SPIR-V builder (`src/gpu/shader/`); SPIR-V emission in progress. No draws rendered yet. |
| Audio | 🟡 Preview only | ATRAC9 player (`src/ui/snd_player.cpp`); no libSceAudioOut HLE. |
| Input | 🟡 Basic | XInput/keyboard → PadButtonState in gpu backend; WPF DualSense HID reader. |
| UI | ✅ Working | WPF .NET 9 frontend (`src/ui_csharp/`) + CLI. |
| Tests | ✅ Good base | 30/30 CTest targets green incl. HLE sync/videoout/AGC/printf/shader suites; freestanding guest ELF smoke tests. |
| Compat infra | 🟡 Seeded | `src/compat/` DB with 6-status taxonomy; `compat/titles/` with 6 titles (PPSA02929 + PPSA07429 at `status-boot`, both running crash-free to the rendering wall); 1860 tracker issues created. |

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

- [x] Audit and complete the syscall table in `src/kernel/syscalls.cpp` against known Orbis/Prospero syscall numbers; stub-unknown-with-log policy. *(Audit done — notable gaps: kqueue/kevent (362/363/461), sigaction family, rlimit, dup. Stub-with-log entries were written but reverted: their presence triggered a latent exit-time fastfail (0xC0000409) in syscall_validation under piped stdout — root cause still open.)*
- [x] Replace return-0 pthread stubs in `src/hle/libkernel.cpp` with real implementations: mutex, condvar, rwlock, sema, event flag, once, tls keys. *(Real implementations in `src/hle/libkernel_sync.cpp`.)*
- [x] Equeue/event queues (`sceKernelCreateEqueue`, wait/poll) — heavily used by system modules. *(User events + minimal edge-armed read events; kqueue/kevent syscalls still unimplemented.)*
- [x] Memory: flexible mappings, direct/GPU-visible memory pools, `sceKernelReserveVirtualRange`, mprotect semantics — use VirtualAlloc placeholder regions (extend `src/memory/memory.cpp`).
- [x] Signal/exception translation: map guest FreeBSD signals ↔ Windows SEH through the VEH path in `src/kernel/kernel.cpp`. *(Document-only so far: mapping policy recorded in a comment block at the VEH; no guest signal delivery yet.)*
- [x] Clock/time accuracy (`sceKernelGetProcessTime`, rtc) — games depend on monotonic timing early. *(Shared QPC origin in `src/kernel/guest_clock.cpp`; GetProcessTime/Counter/Frequency HLE + clock_gettime all use it.)*
- [x] Tests: extend `guest_syscall_smoke` guest ELFs to cover pthread + equeue + mmap scenarios. *(Covered host-side instead: `tests/kernel_sync_tests.cpp` drives mutex/cond/rwlock/TLS/sema/event-flag/equeue/clock directly.)*

## Phase 3 — Core HLE Modules

*Goal: the stub layer every game touches, in priority order. Workflow: stub aggressively, log unimplemented, implement what target games actually call (shadPS4 model).*

- [x] libSceSysmodule (module load/unload routing to the loader). *(Fake-bookkeeping model in `src/hle/libsysmodule.cpp`: id registry + known id→name map, loads faked with success; real PRX loading intentionally out of scope.)*
- [x] libSceLibcInternal (malloc family, stdio, string, setjmp) — decide HLE vs load-real-PRX per function. *(HLE guest heap in `src/hle/liblibc.cpp`: malloc/free/calloc/realloc/memalign/aligned_alloc/posix_memalign over 16 MiB arena chunks with a free list; replaces the leaky page-per-call libkernel versions.)*
- [x] libSceUserService (user profiles, login state — mostly canned responses) — `src/hle/libuser_service.cpp`.
- [x] libSceSystemService (param queries, status events). *(Canned offline set in `src/hle/libsystem_service.cpp`: ParamGetInt map, GetStatus, DisplaySafeAreaInfo, HideSplashScreen.)*
- [x] libSceNpManager / libSceNpCommon — offline stubs returning "not signed in". *(`src/hle/libnp.cpp`: offline-but-signed-in model — GetState=1, OnlineId "Player", Trophy2 context/handle ids, GameIntent/UniversalDataSystem success stubs.)*
- [x] libSceSaveData (dialog-free direct API first) — `src/hle/libsavedata.cpp`; common-dialog + save-data-dialog status flow also implemented.
- [x] libSceVideoOut real flip/queue model (feeds Phase 5 presentation). *(Per-handle port state, 16 buffer slots, 60 Hz vblank pump thread, AddFlip/AddVblankEvent wired to the libkernel equeue, SubmitFlip + 40-byte GetFlipStatus in `src/hle/libvideoout.cpp`.)*
- [x] Auto-generate stub registration from the NID database so every known export at least logs-and-returns. *(`HLE::RegisterNidDbStubs` in `src/hle/hle.cpp`, fed by `Common::EnumerateNidEntries`; runs last in `HLE::Initialize` and never overrides real registrations.)*

## Phase 4 — First Game Bring-Up

*Goal: one simple commercial title reaches menus.*

- [x] Pick 1–3 target titles from `Games/` (prefer smallest/simplest; record pick + rationale in compat DB) — PPSA02929 (Dreaming Sarah) primary, PPSA07429 (LOST EPIC) secondary; see `compat/titles/`.
- [x] Drive boot to first unimplemented blocker; fix; repeat — log every iteration as a compat report (`src/reports/`). Iterations logged in `ppsa02929_run*.log`; blockers fixed so far: save-data/user-service/common-dialog contracts, AGC register defaults, libc `_Getptolower`, libScePad libkernel NIDs, `/app0` path translation.
- [x] Real filesystem view for games: mount game PFS + app0/savedata path translation. (Partial: `/app0` → eboot directory, `/savedata0` → host save-data dir shared with the libSceSaveData HLE, and relative opens resolved against the package root — all in `Kernel::TranslateGuestPath`; guest-visible PFS mount still missing, games run from extracted dumps.)
- [x] Pad input end-to-end: DualSense/XInput → libScePad state → game reads it. *(XInput/keyboard → `GPU::GetCurrentPadState()` → `scePadReadState` in `src/hle/libpad.cpp:76`; Dreaming Sarah calls `scePadInit`/`scePadOpen` via the libkernel NIDs — it crashes at the AGC wall before its first poll.)*
- [x] Per-title config overrides exercised in anger (`src/config/`). *(`pcsx5_config/titles/PPSA02929.json` raises log level to Debug; `main.cpp` logs the active override at boot and applies it via `ConfigService::EffectiveFor`.)*
- [x] Milestone: target title renders menu or reaches "needs GPU" wall — documented in compat tracker. *(Wall reached and breached: Dreaming Sarah runs indefinitely crash-free through full renderer init, submitting DCB draws and flips — window black because draws are not yet executed on the host (Phase 5 M3 slice 2c/3); its real VS/PS shaders already translate at draw time to driver-valid SPIR-V. LOST EPIC runs 3+ minutes crash-free through Il2CppUserAssemblies.prx load. Documented in `compat/titles/`.)*

## Phase 5 — GPU (the long pole)

*Goal: translate GNM/PM4 to Vulkan. Expect this to be the largest single investment.*

**Status (2026-07-19): in progress — M0/M1/M2 done; M3 slices 0/1/2a done (2c in flight).** Reference: SharpEmu (`sharpemu_clone/`, synced to origin/main 0f224ec) — AGC ABI, PM4 semantics, and the RDNA2→SPIR-V translator being transliterated to C++. Live state per title in `compat/titles/`; milestone ledger in `PROGRESS.md`; next steps in `PENDING.md`.

- [x] PM4 command-buffer parser: packet dispatch loop, register writes, draw/dispatch packets — *in `src/hle/libagc.cpp` (M0): 20+ real `sceAgcDcb*` PM4 builders + submit walker (`sceAgcDriverSubmitDcb/Acb/SubmitMultiDcbs`) with Cx/Sh/Uc register shadow state; DMA fills/copies executed into guest memory; RFlip forwarded to videoout.*
- [x] Clear + present path: render-target clear and swapchain blit — first visible output. *(`src/gpu/vk_context.cpp` + `src/gpu/vk_present.cpp` (M1): volk-style dynamic loading (no SDK), swapchain, guest-FB upload → `vkCmdBlitImage` → present; GDI DIB fallback retained. Pixel-correct presents proven via `tests/vk_present_smoke.cpp`; Vulkan-clear helper ready for the bound-RT model.)*
- [x] `sceAgcCreateShader` shader ABI — *(M0: header validation, self-relative pointer relocation, PGM_LO/HI patching, real register-defaults tables; unblocked the boot wall.)*
- [ ] Resource translation: guest GPU memory → Vulkan buffers/images — **in progress** *(M3 slice 1, `2e165a7`: per-draw scalar-state evaluator (`src/gpu/shader/gcn_eval.{h,cpp}` — symbolic execution of the shader scalar stream over a path stack with EXEC/SCC, supplemental fall-through discovery) resolves texture/buffer descriptors from the register shadow's user-data banks; verified in-game (sprite texture + vertex/constant buffers resolve to real guest addresses). M3 slice 2a: dynamic descriptor-driven `BufferLoadFormat*` decode in the translator (unified-format table, per-component bit layouts, unorm/snorm/uscaled/sscaled/uint/sint/float/mini-float conversion). Remaining: guest RT image model, texture upload, descriptor sets.)*
- [x] Shader recompiler: RDNA 2 ISA → SPIR-V — *(`src/gpu/shader/`: decoder for all encoding families, AGC/metadata parser, SPIR-V module builder (M2.1); SPIR-V emission (M2.2, `c923dd6`) in `gcn_translate.{h,cpp}` + `gcn_translate_alu.cpp` — guided transliteration of `SharpEmu.ShaderCompiler.Vulkan`: PC-dispatcher control-flow model, single-lane wave semantics (VCC/EXEC as per-lane bools synced with s106/s126), SGPR/VGPR register files, SDWA source/destination modifiers, compressed exports, VInterp, image-sample/scalar-load/buffer-load paths with bounds-checked storage-buffer access. Full corpus translates: 81/81 shaders, and all 81 modules are accepted by the NVIDIA ICD via `vkCreateShaderModule` (`shader_dump --translate-corpus` / `--validate-spv`). M3 slice 0 (`be2571a`): draw-time shader discovery + translation proven in-game (VS/PS fetched from the register shadow at draw packets, translated, driver-validated). Rejected by design (absent from the corpus; fail loudly): DPP, VOP3P packed-f16, DS, global-memory, buffer store/atomic, storage image load/store, offset/compare image samples.)*
- [ ] Pipeline state → Vulkan pipelines; descriptor translation; samplers — **in progress** *(M3 slice 2: vk_context function table extended with pipeline/descriptor/sampler/render-pass/cmd-draw entry points; per-draw runtime-SGPR model (initial-scalar buffer) in flight (slice 2b), then pipeline build from the Cx/Uc shadow (topology, blend, raster, viewport/scissor) + descriptor sets (slice 2c) and present-from-GPU-image (slice 2d).)*
- [ ] Texture formats/conversion table; tiling/detiling.
- [ ] Compute queue support.
- [ ] Shader cache (disk) + async pipeline compilation.
- [ ] Validation strategy: capture/replay tool for PM4 streams + golden-image tests. *(Partial: `tests/shader_dump.cpp` corpus translator + driver validator (`--translate-corpus` / `--validate-spv`) — 81/81 corpus shaders emit SPIR-V accepted by the NVIDIA ICD; 54 golden shader pairs (RDNA IR + working SPIR-V) captured from the reference emulator for cross-checking; golden-image capture/replay still open.)*

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
