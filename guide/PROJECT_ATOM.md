# PCSX5 — PROJECT_ATOM (AI memory / project blueprint)

Load this into context before any large engineering task on PCSX5. It is the
concise, versioned truth about structure, ownership, invariants, and where to
touch for each kind of work. It intentionally does not repeat every class —
see `COMPONENT_MAP.md` for the full inventory.

## What PCSX5 is
PlayStation 5 emulator, **native x86-64 direct execution** + **High-Level Emulation
(HLE)**. C++20, MSVC `/W4 /WX`, GPL-2.0. Windows-first; Vulkan graphics; C# WPF GUI
hosting the native core in-process.
- Guest and host are both x86-64 → **no JIT/dynarec**; guest code runs natively.
- Guest memory is **identity-mapped** (guest_addr == host VA). Direct host pointers.
- Interception: `syscall` patches to `INT3` → VEH; HLE imports get executable thunks
  that jump to a common dispatcher (`dispatcher.asm`).
- Build: `cmake --build build --config Release --target pcsx5_cli`. Tests: `ctest
  --test-dir build -C Release`.

## Invariant rules (do not break)
1. Backends stay inside their own dir and are reached ONLY through the *L interface:
   GPU via `GpuDevice` (GAL), audio via `AudioDevice` (AAL), input via
   `InputBackend` (IAL), video via `VideoDecoder` (VDAL), OS via `Platform::` (PAL).
2. Per-game fixes live in `src/hle/` or `src/compat/`, never in `src/gpu/`/
   `src/cpu/`/`src/memory/` unless it's a genuine core bug.
3. Backend headers (`vk_*.h`) are never included outside their implementation dir.
4. Every new option registers in `src/config/config.h` and is consumed by the
   subsystem that owns it.
5. Guest exceptions are handled via VEH, not C++ `throw`; no RTTI (`/GR-`).
6. `src/kernel/kernel.cpp:286` `Kernel::TranslateGuestPath` is the single
   guest-path→host-path translator (`/app0/`, `/savedata0/`).

## Subsystem layout (dir → owner concern → seam)
| Path | Owners | Extend here for |
|---|---|---|
| `src/common/platform/` | OS wrappers (PAL) | OS port, memory strategy |
| `src/loader/` | PKG/PFS/SELF/ELF, modules, resolution, NID→name | file loading, modding, decode |
| `src/kernel/` | VEH, syscalls, fd table, threads, path translate | syscalls, byte-patches, path override |
| `src/cpu/` | guest thread lifecycle, syscall adapter | thread work, future JIT/step-trace |
| `src/memory/` | guest addr space, protection, write-tracking | watchpoints, memory strategy |
| `src/hle/` | userland lib emulation (libc, libkernel, libSce*) | **game compat fixes** |
| `src/hle/audio/` | AAL backends (`AudioDevice`) | native audio endpoint |
| `src/gpu/` | GAL (`GpuDevice`) + Vulkan + shader translate | **graphics/resolution/native GPU** |
| `src/gpu/input/` | IAL backends (`InputBackend`) | native controller/input |
| `src/gpu/shader/` | RDNA2/GCN→SPIR-V | shader/HDR output |
| `src/media/` | VDAL (`VideoDecoder`) | codecs/cutscenes |
| `src/compat/` | per-title JSON DB | game-specific patches/status |
| `src/config/` | JSON config schema | any new option |
| `src/ipc/` | shared-mem frame handoff to UI | GUI frame display |
| `src/ui_csharp/` | WPF desktop GUI | frontend |
| `src/core_api.cpp` | Init/Load/Run/Stop/Shutdown | frontend/host binding |

## Touch-only-these tables

**Game modding** → `src/kernel/kernel.cpp` `TranslateGuestPath` (add mod_dir
override) + `src/config/config.h` + `src/compat/`. Leave `src/gpu/`/`src/cpu/` alone.

**Resolution / upscaling / render enhance** → `src/gpu/` only: `vk_present.cpp`
(swapchain/viewport scale), `fsr_upscale.h` (FSR/RCAS), `src/gpu/shader/` (spirv).
Leave `src/hle/` and `src/cpu/` alone.

**Native GPU backend (D3D12/etc.)** → implement `GpuDevice` (`src/gpu/gal.h`) in a
new `src/gpu/d3d12/`; register in GAL factory; config `GraphicsConfig.renderer`.

**Native input** → `src/gpu/input/`: implement `InputBackend` + factory
(`input_backend.cpp:15` Create) + `InputConfig.backend`.

**Native audio** → `src/hle/audio/`: implement `AudioDevice` + factory
(`audio_device.cpp:32` Create) + `AudioConfig.backend`.

**New syscall / byte-patch** → `src/kernel/`.

**Guest instruction trace / step-tracer / write-watchpoint** → `src/cpu/` +
`src/kernel/` (VEH) + `src/memory/` (`TrackGuestWrites`). Not HLE/GPU.

## Known current limitation (important — avoid re-spending effort)
Dreaming Sarah (PPSA02929) and similar boot + render the splash then crash in the
game's OWN native content-load (uncaught `std::invalid_argument` from the Construct
C2 runtime). Verified: every emulator HLE/subsystem is byte-faithful
(strtod/strlen/memcpy/strncpy/__error; C2 char-class rodata at guest 0x8102fe410
loads correctly; `dhK16CKwhQg` return/endptr theories refuted; `data.js` reads back
the full 1,245,105 bytes). The failure is guest-native record-building that bypasses
all HLE. **There is no emulator-HLE patch for this title**; the way forward is the
guest instruction trace/step-tracer (see table above) — build that instead of
re-litigating the same hypotheses. (Fuller writeup: `.work/datajs_parse_crash_findings.md`.)

## Naming / style (must-match for new code)
`CamelCase` types, `snake_case` functions/vars, `kCamelCase` constants, `g_` globals,
`m_` members, `t_` thread-locals. `#pragma once`. C++20. No RTTI. SEH only in
POD-only leaf functions.
