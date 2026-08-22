# PCSX5 Repository Audit

Date: 2026-08-22
Scope: read-only audit of the repository. Nothing was modified.

## 1. What this repository is

PCSX5 is a Windows x64 PlayStation 5 emulator. The guest is also x86_64
(AMD Zen, RDNA2 GPU), so guest code runs by **direct execution** — no JIT.
The core maps guest VAs to host memory via VEH-handled fault pages and uses
a small amount of hand-written MASM (the SysV↔Win64 calling-convention bridge)
plus software fallbacks for AMD-only instructions (BMI1/BMI2/ABM/SSE4a).

Layout at a glance (~155k native LOC by directory):

| Subsystem | ~LOC | Role |
|---|---|---|
| src/hle | 17,400 | HLE services + guest libc/kernel-lib NIDs (largest, most entangled) |
| src/gpu | 7,600 | Vulkan backend, RDNA2 shader→SPIR-V translation, GAL, input backends, FSR |
| src/kernel | 5,850 | kernel.cpp, syscalls, threads, guest TLS, guest VA allocator, FD table |
| src/loader | 4,700 | ELF/SELF/PKG/PFS loading, module dependency graph |
| src/core_api.{h,cpp} | ~1,900 | the C ABI composition root (plus src/main.cpp the CLI shim) |
| src/memory | 1,000 | `Memory::` region-tracked VA manager + VEH fault handler + write-tracking |
| src/media | 1,200 | Bink2 / CRI-USM / FFmpeg decoders behind VideoDecoder (orphaned) |
| src/ui_csharp | (C#) | WPF shell, discourse of the IPC + CoreBridge dual transports |

The WPF UI talked via CoreBridge P/Invoke originally, but today the active
runtime path is **out-of-process**: the UI spawns `pcsx5_cli.exe` with
`--ipc-map/--ipc-pipe/--headless`, the core renders frames into a shared
memory map, the UI polls a frame counter and blits into a WPF `WriteableBitmap`.
CoreBridge still exists and is partially wired (PS-button stop, `pcsx5_get_last_error`)
but the in-process path is no longer what ships. The repo thus has **two live
core-integration transports**, only one practically exercised.

## 2. What demonstrably works (evidence, not claims)

These are the claims the repository's own artifacts back:

1. **PKG extraction** — `pkg_ps5` ctest + dedicated CI step; README marks it
   green. Best-evidenced feature.
2. **Guest boot-through-module-linking** for real titles. `boot_baseline.log`
   (PPSA02929) reaches "First guest draw executed"; `.work/astro_boot_progress.md`
   (PPSA21564) executes guest code across 3 modules (~520 imports) before dying.
3. **RDNA2→SPIR-V translation corpus** — "81/81 corpus shaders translate;
   81/81 accepted by NVIDIA ICD" (`shader_tests`, ~58KB).
4. **Vulkan present pixel-correctness** — golden fixtures under
   `tests/golden/pm4_synth/` exercised by the `pm4_golden` ctest.
5. **Headless teardown** — `boot_test.log` (TEST0001) shows clean init/shutdown
   and correct missing-ELF failure handling.
6. **Unit-test surface** — ~40 registered ctests across loader, memory, kernel,
   HLE, GPU, reports, config, compat, TLS, replay.

## 3. What does not work (the evidence base)

- **No title reaches a menu in any log artifact.** Peak claimed state
  (PROGRESS.md, 2026-07-24) was Dreaming Sarah "boots indefinitely crash-free,
  draws+flips". The freshest objective record — `.work/autopilot_history.jsonl`
  entry 2026-08-21 — scores the same game `uncaught-exception` at the
  Construct `data.js` parse wall (`markers:["pthreads"]`,
  `content_loaded:false, menu_reached:false`).
- **PPSA02929 (Dreaming Sarah)** — status `intro`. Splash renders, then guest
  throws an uncaught C++ exception in the Construct runtime
  ("type must be number, but is null"). Current blocker: the HLE
  `liblibc` exception unwinder's `.eh_frame` evaluation (see §5.2).
- **PPSA07429 (LOST EPIC)** — status `boot`. Dies writing `[RAX]` with `RAX=0`
  right after the AGC buffer-mapping sequence: the `sceAgcCreateShader` stub
  returns 0 so the shader handle the game dereferences is null.
- **PPSA21564 (ASTRO BOT)** — real dump; boots past TLS/module init then calls a
  NULL function pointer stubbed via the data-NID `#A#B` auto-stub path.
- **PPSA01668, PPSA10112, PPSA20591, PPSA23885** — `status: nothing`, never
  tested (no logs, no last_tested timestamps).
- The compat DB is internally inconsistent: PPSA02929 says `intro` while its own
  notes say "Menus not yet reached" — the freshest autopilot run agrees with the
  notes, not the status field.

### "Working" README rows that lack runtime evidence

- Audio output: no log artifact shows actual audio path activity.
- Regression infra: `tests/regression_manifest.json` is an empty template
  (`passed:0 failed:0 total:0 runs:[]`); `replays/` has a README and zero replays;
  `LastTest.log` (Jul 22) records **zero** test results.
- Bink2 (PENDING.md "Fully functional") but CRI-USM is a stub and no decode log exists.

**Literal rule from `00-evidence-and-scope.md`:** every compatibility workaround
must state title/build, symptom, evidence, root-cause confidence, workaround,
test coverage, and removal condition. That discipline is not practiced yet.

## 4. The architecture today vs. the target

The target (ADR 0001, ARCHITECTURE.md) is a layered core: `CPU -> memory -> kernel/HLE
-> loader -> GPU/audio/input -> runtime`, dependency-graph clean, headless-testable,
WPF a thin shell. The actual graph (verified dependencies):

```text
  loader ← kernel ↔ {hle, cpu} → memory → (nothing)
               │                          │
               └── everything ──► gpu.h facade
   gpu → ipc_gpu_bridge.h (C++17 inline fn-pointer globals) only
```

Key discrepancies:

1. **kernel ↔ hle ↔ cpu .cpp-level cycles.** `cpu/cpu.cpp` includes `hle/hle.h`
   (to reach dispatcher symbols) while `hle/libkernel.cpp` includes `cpu/cpu.h`.
   `kernel/kernel.cpp` includes `hle/hle.h` and HLE libs include `kernel/*.h`.
   These are not header cycles (they compile) but they prevent any subsystem
   from being owned/tested alone and they defeat the layered `Rule 40` model.
2. **HLE → GPU internals, not the facade.** `hle/libagc.cpp:21-27` includes five
   GPU internals (`gcn_decode|gcn_eval|gcn_translate`, `shader_cache`,
   `gfx10_state`, `vk_draw`) and touches `DrawState g_ds` + `g_draw_mutex`
   directly. This is the worst layering violation and it sits exactly on the
   current boot blockers (§3: AGC shader handle null = the game hits the
   `g_ds`/submission contract the stub doesn't honor).
3. **`gpu.h` facade vs `gal.h`/`GpuDevice` — two front doors.** `gpu.h` (132 lines,
   thin, clean) drives `vulkan_backend.cpp`; `gal.h`/`GAL::GpuDevice`
   (gal_backends.cpp + vulkan/vulkan_device.cpp) wraps the same `vk_*` code "for
   future backends / software path". GDI device header exists but is unreferenced
   and not built. Two GPU abstractions, one backend actually implemented.
4. **Two memory managers.** `Memory::` (region-tracked, VEH fault, write-tracking,
   pool allocator, ~35 consumers) and `Kernel::` (bump+mmap allocator in
   `kernel/memory.cpp`, used only from kernel.cpp) both VirtualAlloc from the
   **same guest VA space** with no shared coordination.
5. **Two TLS paths.** `kernel/tls.{h,cpp}` implements the real
   `GuestTlsContext::Configure/Translate`; `kernel/tls_patch.{h,cpp}` is a
   no-op stub, yet the wired-in include set (`cpu/cpu.cpp:18`, `kernel/kernel.cpp:7`)
   picks the stub. A working implementation neighbors a selected no-op.
6. **Two C# transports** (CoreBridge in-process P/Invoke vs out-of-process IPC) —
   §1.
7. **Header leakage:** `gfx10_state.h:18` includes `<vulkan/vulkan.h>`, pulling
   backend headers into HLE (libagc) even though `gal.h:5-7` promises backend
   headers never cross ownership boundaries. `reports/reports.h:20` includes
   `../hle/hle.h` at header scope to reuse `ImportStats` (dragging `<csetjmp>`
   into every report consumer).
8. **Orphans:** `src/media/*` (listed in core sources, zero native callers),
   `src/system/*` (only a test exe), `src/compat/*` (tools/tests only),
   `gpu/gdi/` (unreferenced), `fsr_upscale` (built, no includers).
9. **CMake source duplication:** the same `.cpp` files are added to
   pcsx5_core multiple times (e.g. `libappcontent.cpp`, `librtc.cpp`,
   `libfiber.cpp`, `libregmgr.cpp` at lines 232-244 and again 239-242;
   `input_bot.cpp` appears ~10 times across target lists). Duplicate object
   definitions; MSVC tolerates it today and it is a latent ODR landmine.

## 5. Global state and ownership

~90 distinct file-scope/namespace-scope mutable state clusters; exactly one
classic singleton (`LuaInit::SubsystemRegistry::Instance()`). Highest risk:

- `hle/hle.cpp` (~20 clusters): import registry, thunk page, stop/exit `setjmp`
  state shared across all guest threads.
- `gpu/vulkan_backend.cpp` (~25): `g_window (GLFWwindow*)`, `g_vk (VkContext*)`,
  pad state, XInput fn-pointers, input recorder.
- `kernel/kernel.cpp` (~20): thread map, module registry, VEH handler, heartbeat.
- **Exported (non-static) globals** that occupy the linker namespace and can
  collide across the CMake's many test TUs:
  - `memory/memory.cpp:28-33` (`g_regions`, `g_fault_handler`, `g_fault_veh`, …)
    and the pool `g_pool_base/g_pool_used/g_pool_ok` (non-static by design so the
    pool init call can reach them — but they're exported symbols).
  - `gpu/vk_draw.cpp:110` `DrawState g_ds` + `std::mutex g_draw_mutex`.
  - `gpu/vk_present.cpp:12-13` `g_config_vsync/g_config_vrr`, `:61-62`
    `PresentState g_ps; g_present_mutex`.
  - `core_api.cpp:72` `CoreState g_state`.

Windows-specific code is broad: 49 native files include `windows.h`; the whole
kernel/, most of HLE, gpu backends/input, ipc, media, config, compat, core_api,
main. Portable-by-construction: `common/types|crypto|nid`, `loader/*`,
`memory.h`, `gal.h`, `gpu.h`, `lua_init.h`, `reports.h`, `system.h` (guarded).

### Dependency-governance status (Rule 60 / Rule 05)

- GLFW is honestly narrow: only `gpu/vulkan_backend.cpp` (window lifecycle) and
  `gpu/vk_context.cpp:92` (surface). Fetched via FetchContent.
- ImGui: no `imgui.h` include in native src; comment/`#ifdef` references only.
  ImGui lives in the separate pcsx5_ui target. Clean.
- Lua: single site (`lua/lua_init.cpp`) driving scripted init ordering from
  `default_init.lua`; invoked from core_api. No game-facing scripting.
- nlohmann/json: 3 documented sites (import-report export, input replay, param.json).
- NAudio: the only managed PackageReference, used solely for DualSense
  WASAPI speaker/mic test tones. ARCHITECTURE.md flags duplicate native audio
  as a re-evaluation candidate; today it is confined to the UI, not duplicated
  in core.
- FFmpeg/Bink2/CRI-USM: behind `video_decoder.{h,cpp}` but the whole media
  module is orphaned from the runtime; ADR 0003 covers the isolation rule.

## 6. Bug-fixing hotspots tied directly to these findings

The **data-NID auto-stub** (`#A#B` export stubs returning 0) is the common
failure ancestor for PPSA07429 and PPSA21564: unimplemented HLE symbols produce
silent-0 definitions, the game dereferences them, and the crash surfaces as a
NULL fault at a *guest* address that no HLE annotation explains. The missing
AGC contract (what `sceAgcCreateShader` must write into the mapped buffer —
relocated pointer fields, PGM_LO/HI program registers, header pointer) is
documented in PPSA02929 notes with a full ABI spec, not implemented.

## 7. Technology debt and dangerous areas (ranked)

| # | Finding | Why it bites | Evidence/confidence |
|---|---|---|---|
| 1 | HLE↔kernel↔cpu cycles | subsystem cannot be owned/tested alone; blocks every boundary in Rules 40/50 | include scan; cycles at .cpp level, compile today |
| 2 | libagc pierces gpu layer → vk_draw `g_ds` | sits exactly on AGC boot blockers; gal.h isolation is cosmetic | verified `libagc.cpp:21-27`; high |
| 3 | two memory managers over the same VA | double-management; a fix in one can break the other invisibly | verified both VirtualAlloc-backed; high |
| 4 | stub TLS path wired, real tls.cpp orphaned | silent no-op where behavior was implemented | verified include set; high |
| 5 | AGC / data-NID auto-stub → NULL-deref crashes | makes the top-3 title failures ambiguous: real bug vs missing stub | compat notes; high |
| 6 | 2 GPU abstractions, 1 backend | migration drag; GDI dead code | include/build scan; high |
| 7 | 2 C# transports | the shipped path is IPC but CoreBridge still holds stop/last-error, so both must be kept correct | MainWindow.cs verified; medium |
| 8 | 90 globals incl. exported ones | cross-TU ODR risk, no clear ownership, hard to reason about lifecycle | scan; medium |
| 9 | CMake duplicated sources | ODR landmine, non-deterministic linking, slow builds | CMakeLines 232-244, verified dupes; high |
| 10 | orphan modules (media/system/compat/gdi/fsr) | dead claims of capability; "working" without runtime evidence | ownership scan; medium |
| 11 | reports.h → hle.h header coupling | dragging `<csetjmp>` into report consumers | verified; medium |
| 12 | no regression evidence actually runs | `LAST_TEST.log` empty, manifest empty, replays empty → ctest "green" claims unverifiable | artifacts; high for CI trust |

## 8. Recommended staged path (consistent with Rule 50 and REFACTORING_PLAN.md)

The work splits into two kinds: **evidence work** (no behavior risk) and
**single-subject migrations** (one boundary at a time, build+test gate every step).

### Evidence backlog (do first, cheap, zero risk)
1. Reproduce the "30/30 ctest green" claim: configure Release+Debug, run ctest,
   record actual pass/fail + the skipped-clang tests. Archive as a baseline.
2. Regenerate the autopilot/compat state regularly; reconcile PPSA02929
   `status: intro` vs notes so the DB is internally consistent.
3. Fill the AGC boot-blocker spec (what `sceAgcCreateShader` must write) with a
   characterization test against the *existing stub*'s observable behavior, so
   the coming implementation has a regression net.
4. Delete annexes only after noting what they claim to do and that nothing
   exercises them: `gpu/gdi/`, unreferenced `fsr_upscale` files if unreferenced,
   `src/media` from core sources if truly orphaned, no-op `tls_patch` once the
   real TLS path is selected.

### Migration theses (one at a time, each is 1 ADR + small slices)
1. **Owner the memory boundary first** (REFACTORING_PLAN Phase 4): move
   `Kernel::` allocate/map/break consumers onto `Memory::`, delete
   `kernel/memory.cpp` only when equivalence is demonstrated. This removes the
   double-management hazard before bigger refactors lean on it. **This is the
   recommended first task.**
2. Wire the real TLS path or delete the stub; add a `tls_context` test that
   exercises `GuestTlsContext::Configure/Translate` so the no-op can't silently
   return.
3. Break the kernel↔hle cycle at one boundary (e.g. move ImportStats out of
   hle.h so `reports.h` stops pulling it in; flip one of the two directions to
   depend on the other). Candidate: define the few symbols kernel needs from HLE
   behind a `KernelHleBridge` with exactly those calls, then invert includes.
4. Close the libagc→gpu leak: give AGC a narrow `GpuAgcSession` API exposing
   exactly what AGC needs (create shader → submit via a callback), leaving
   `gk_draw` internals in GPU. This is the second most valuable target because
   it unblocks PPSA07429/PPSA21564 **and** removes the Rule-40 violation at once.
5. Pick one GPU front door (gpu.h) and make gal.h either the adapter-writer's
   interface or delete it; keep GDI out unless a software path is a real goal.
6. De-duplicate CMake source lists as a mechanical, behavior-free change.
7. Remove the data-NID silent-stub trap: make auto-stubbed imports fail loudly
   (log `STUBBED IMPORT`, mark report) when they're actually *called*, so NULL
   derefs are attributable.

### Use of compos
Rule 90 (compatibility as measured matrix) is the correct frame: everything in
§3 should become rows in the compat DB with the credentials the rule demands
(title, build, stage, tested config, last-known-good commit, failure signature,
confidence, regression test) — not prose in notes or README green rows.

## 9. Bottom line

The repo is a large, working direct-execution experiment whose governing docs
(Rules 40/50/60/90) are ahead of the code: ownership boundaries, dependency
discipline, and the compatibility matrix are **documented but not yet enforced**.
The safe next step is not another feature — it is the evidence backlog (§8a)
followed by one owned boundary migration (memory unification). Nothing here
requires a rewrite; every migration is small and reversible if each slice builds,
tests, and its behavioral change is deliberate.
