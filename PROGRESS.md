# pcsx5 progress (as of 2026-07-19)

## Phases 0–4 — complete

Kernel (real pthread/equeue/sema/event-flag sync, demand-commit memory,
unified clock), broad HLE (savedata, user/system services, NP, sysmodule,
libc heap + printf, videoout flip model), 553+ NID-DB stubs, per-title
config, `/app0`/`/savedata0` filesystem translation. 30/30 ctest green.

## Phase 5 (GPU) — milestone status

| Milestone | Status | Proof |
| --------- | ------ | ----- |
| M0 AGC HLE | done | `sceAgcCreateShader` real ABI, register-default tables, 20+ PM4 builders, submit walker with Cx/Sh/Uc shadow state (`src/hle/libagc.cpp`) |
| M1 Vulkan backend | done | Dynamic loading (no SDK), swapchain present proven pixel-correct, GDI fallback (`src/gpu/vk_*`) |
| M1.5 boot blockers | done | Both games boot crash-free (see below) |
| M2.1 shader foundation | done | Full RDNA2 decoder + metadata parser + SPIR-V builder (`src/gpu/shader/`); corpus of 81 real shaders decodes |
| M2.2 SPIR-V emission | done, `c923dd6` | **81/81 corpus shaders translate; 81/81 accepted by NVIDIA ICD** (`shader_dump --translate-corpus` / `--validate-spv`) |
| M3.0 draw-time translation | done, `be2571a` | In-game: VS+PS found via register shadow at draw, translated, driver-validated |
| M3.1 scalar evaluator | done, `2e165a7` | In-game: per-draw texture/vertex/constant buffer descriptors resolve to real guest addresses |
| M3.2a vertex-format decode | done (uncommitted) | Dynamic descriptor-driven `BufferLoadFormat*` decode in translator; 81/81 re-validated |

## Game status

- **Dreaming Sarah (PPSA02929)**: boots indefinitely crash-free, submits
  draws + flips every frame, Vulkan presents the guest FB. Window black —
  draws aren't executed yet (that's M3 2c/3).
- **LOST EPIC (PPSA07429)**: boots through IL2CPP assembly loading.

## Reference assets gathered

- SharpEmu install (`I:\Installed Games\sharpemu-win64-fbf2c2d`) with
  Dreaming Sarah playable: 2 captured reference logs
  (`sharpemu_ppsa02929_ref.log`, `_run2.log`) and **54 golden shader pairs**
  (RDNA IR + working SPIR-V) in its `shader-dumps/`.
- SharpEmu source clone (`sharpemu_clone/`) — blueprint for everything above.

## Known non-blocking issues

- ~47 first-chance `VCRUNTIME140` memcpy AVs reading guest heap — caught by
  SEH guards, log noise.
- One `sceSysmoduleIsLoaded` AV caught by the dispatcher SEH guard.
- Translator rejects loudly (by design): DPP, VOP3P packed-f16, DS,
  global-memory, buffer store/atomic, storage image load/store, compressed
  export precision shadow, offset/compare image samples.
