# PCSX5 High-Priority Boot Blockers

> Derived from [boot_analysis.md](boot_analysis.md) — comparison with KytyPS5 boot pipeline.
> Last updated: 2026-07-24

---

## Priority Legend

| Icon | Meaning |
|------|---------|
| 🔴 | Blocks boot — no game progresses past this point |
| 🟡 | Degrades boot — game may boot but crashes/hangs soon after |
| ⚪ | Infrastructure — needed for future titles |

---

## 🔴 P1: GPU Window/GLFW Init Hard-Blocks Boot

**Problem**: `GPU::Initialize()` at [vulkan_backend.cpp:507](src/gpu/vulkan_backend.cpp) returns `false` if `glfwInit()` or `glfwCreateWindow()` fails. This propagates up through `LuaInit::RunDefaultInit()` → `pcsx5_init()` returns -1. No game boots without a functioning GLFW window.

**Fix**: Add a `headless` mode config option (and env-var override `PCSX5_HEADLESS=1`) that skips GLFW window creation when the game is being tested from CLI. The GAL backend auto-probe already falls back to the Null backend when no window is available, but `Initialize()` itself never reaches `GpuDevice::Create()` because GLFW window creation is unconditional.

| File | Impact |
|------|--------|
| [vulkan_backend.cpp](src/gpu/vulkan_backend.cpp) | `Initialize()` bails on GLFW fail |
| [gal.cpp](src/gpu/gal.cpp) | GAL has Null backend but never gets called |
| [core_api.cpp](src/core_api.cpp) | Returns -1 on subsystem init failure |

**Status**: ⬜ Not started

---

## 🔴 P2: GPU Subsystem Init Is Serial in the Lua/C++ Init Chain

**Problem**: The Lua/C++ subsystem registry at [lua_init.cpp:237](src/lua_init/lua_init.cpp) registers GPU with dependencies `{"Memory", "HLE", "Kernel"}`. If GPU init fails, the WHOLE chain fails and boot stops — there is no concept of "optional" subsystems.

**Fix**: Make the GPU init failure non-fatal in the C++ chain. When GPU init fails, log at WARN level and continue. Downstream code (flip queue, render) should check `HasWindow()` before calling GLFW functions.

**Status**: ⬜ Not started

---

## 🔴 P3: Boot-Critical HLE Coverage — libSceSysmodule Dynamic Loading

**Problem**: `sceKernelLoadStartModule()` is implemented at [libkernel.cpp:743](src/hle/libkernel.cpp) but depends on `firmware_modules_dir` being configured. Many games dynamically load PRX modules at boot via `sceSysmoduleLoadModule()` (implemented at [libsysmodule.cpp](src/hle/libsysmodule.cpp)). If the config paths are empty, PRX resolution falls back entirely to HLE stubs, which may not have the right implementations.

**Fix options**:
1. Ship stub PRX files for common system modules (the KytyPS5 approach)
2. Ensure HLE stubs intercept every NID that a booting game might call
3. Add a `--sys-modules=<dir>` CLI flag that auto-configures firmware_modules_dir

**Impact**: Games that call `sceSysmoduleLoadModule()` during boot (most UE4/UE5 titles) will hang or crash if the module is critical for boot.

**Status**: ⬜ Not started

---

## 🔴 P4: Missing param.json / Title ID Fallback

**Problem**: [core_api.cpp:179](src/core_api.cpp) calls `ConfigService::EffectiveFor(g_state.title_id)`, but title_id may be empty if `sce_sys/param.json` (or `param.sfo`) is not found. The analysis reports that KytyPS5 falls back to "UNKNOWN" — we should do the same.

**Fix**: In the `pcsx5_load()` path, when title_id is not set via options, try to read it from `sce_sys/param.json` (or `param.sfo`) in the game directory. Fall back to `"UNKNOWN"`.

**Status**: ⬜ Not started

---

## 🟡 P5: PRX Module Resolution Default Paths

**Problem**: [kernel.cpp:202](src/kernel/kernel.cpp) `ConfigureModuleResolver()` takes `firmware_modules_dir` from config. If the config doesn't set it, PRX resolution for system modules (`libSce*`, `libc`, etc.) can't find real PRX files and falls back to HLE stubs. The NID-database gap filler at [hle.cpp:251](src/hle/hle.cpp) creates log-and-return-0 stubs for every known NID, but some functions need real side-effects (not just return 0).

**Fix**: 
1. Ship a minimal set of stub PRX files from the PS5 firmware
2. OR ensure all common NIDs have proper HLE implementations with correct side effects
3. Add a config auto-setup step that populates firmware_modules_dir with reasonable defaults

**Status**: ⬜ Not started

---

## 🟡 P6: In-Process Mode GLFW Threading Issue

**Problem**: `GPU::Initialize()` calls `glfwInit()` and `glfwCreateWindow()` on the calling thread (the pcsx5 emulator thread). In in-proc mode (hosted in the WPF app), this may run on a thread that WPF controls, causing subtle race conditions or DPI issues.

**Fix**: Ensure the GLFW init/creation happens on the same thread that will pump events (the main window loop in `pcsx5_run()`). Consider deferring window creation until `pcsx5_run()`.

**Status**: ⬜ Not started

---

## 🟡 P7: VEH Exception Handler — Debug Output Noise

**Problem**: The VEH at [kernel.cpp:1156](src/kernel/kernel.cpp) logs every first-chance access violation at INFO level. During normal boot, this generates massive log output ("Instruction bytes at crash RIP...", "Parsing instruction for TLS emulation...") that drowns out actual error messages. The TLS instruction parser at [kernel.cpp:1264](src/kernel/kernel.cpp) also logs each emulated access at INFO level.

**Fix**:
1. Change VEH TLS emulation logging from INFO to DEBUG
2. Suppress repeated "Instruction bytes" logging for the same RIP
3. Add a rate-limited summary counter ("VEH: emulated %llu TLS accesses") instead of per-access logging

**Status**: ⬜ Not started

---

## ⚪ P8: Embedded Default Init Script

**Problem**: The Lua init script embedded in [lua_init.cpp:505](src/lua/lua_init.cpp) (`kDefaultInitScript`) is a static C string that mirrors the C++ `RunDefaultInit()`. When Lua is available, the script runs and calls `RunInitChain()` at the end. But if PCSX5_HAS_LUA is not defined, the C++ fallback `RunDefaultInit()` is used instead, which hardcodes the same subsystem order. However, the `SubsystemRegistry` is thread-safe and supports dynamic registration — so a future Lua script could add new subsystems, but the static C++ chain always runs first.

**Fix**: Make the default behavior:
1. Always try Lua first (if compiled with PCSX5_HAS_LUA)
2. If assets/pcsx5_init.lua exists on disk, load and execute it instead of the embedded script
3. If neither Lua nor init script are available, fall back to the C++ chain

**Status**: ⬜ Not started

---

## 🔴 P9: Boot-Time Diagnostics — What Actually Fails?

**Problem**: When boot fails today, the user sees "FATAL: Failed to initialize subsystems" with no indication of which subsystem failed or why. The SubsystemRegistry at [lua_init.cpp:118](src/lua/lua_init.cpp) logs the subsystem name on init and on failure, but:
1. The inner error message from the failed init might not be printed
2. There's no final "diagnostic summary" that shows the user what to check

**Fix**: Add a comprehensive boot-diagnostic helper that prints:
- Which subsystems initialized successfully
- Which failed and why (the actual error message)
- A suggested action ("Check config file", "Install Vulkan drivers", etc.)
- GPU availability (Vulkan version, GLFW status)
- Whether the game binary (eboot.bin) was found and parsed correctly
- List of unresolved imports (NIDs that the game needs but we don't implement)

**Status**: ⬜ Not started

---

## 🔴 P10: Guest Entry Point / main() Resolution Issues

**Problem**: [kernel.cpp:714](src/kernel/kernel.cpp) attempts to find `main()` via:
1. Symbol table lookup for "main"
2. String table scan
3. Code scan (entry point disassembly analysis)

If all three fail, `main_va` is 0, and the guest execution starts at the ELF entry point without setting `HLE::GetGuestMainAddress()`. The `XKRegsFpEpk` handler at [libkernel.cpp:499](src/hle/libkernel.cpp) then tries to call `InvokeGuestFunction(main_va)` with main_va=0, which could crash or behave unexpectedly.

**Fix**: When main() cannot be located and entry point is the only option, still set the guest main address to a known sentinel and let `XKRegsFpEpk` handle it gracefully.

**Status**: ⬜ Not started

---

## Implementation Order

```
Iteration 1 (current sprint):
  P9 - Boot-Time Diagnostics      → Know where we fail
  P4 - param.json Title ID        → Graceful fallback
  P1 - GPU Headless Mode          → Boot without window
  P2 - GPU Init Non-Fatal         → Continue boot on GPU fail

Iteration 2 (next sprint):
  P5 - PRX Module Resolution      → Better default paths
  P10 - Guest main() Resolution   → More robust entry point
  P3 - Sysmodule Dynamic Loading  → Better HLE coverage

Iteration 3 (backlog):
  P7 - VEH Log Noise              → Clean up debug output
  P8 - Lua Init Script            → External init script support
  P6 - In-Proc GLFW Threading     → WPF hosting fix
```
