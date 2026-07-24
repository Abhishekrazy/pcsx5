---
name: boot-fixes-sprint-1
description: Completed fixes for game boot blockers — headless GPU, title ID fallback, diagnostics, VEH noise
metadata:
  type: project
---

# Sprint 1: Boot-Blocking Fixes (2026-07-24)

Based on [boot_analysis.md](wiki/boot_analysis.md) comparison with KytyPS5, implemented these fixes:

## Fixes Implemented

### P1/P2: GPU Headless Mode + Non-Fatal Init
- [vulkan_backend.cpp](src/gpu/vulkan_backend.cpp): Added `g_headless` mode via `graphics.headless` config or `PCSX5_HEADLESS=1` env var
- GLFW init failure and window creation failure now fall back to headless mode instead of returning false
- `ShouldCloseWindow()` returns `false` in headless mode so the event loop keeps running

### P4: param.json / Title ID Graceful Fallback
- [core_api.cpp](src/core_api.cpp): `pcsx5_load()` now auto-detects title_id from `sce_sys/param.json` when not set via options
- Falls back to `"UNKNOWN"` when no param.json exists (matching KytyPS5 behavior)
- Per-title config overrides now work from load stage onward

### P7: VEH Log Noise Reduction
- [kernel.cpp](src/kernel/kernel.cpp): Changed ~12 TLS emulation logs from `LOG_INFO` to `LOG_DEBUG`:
  - Per-access TLS read/write emulation details
  - Instruction bytes at fault RIP
  - SIB base and mod/rm decode
  - TlsPatch retry messages
  - Kept TLS alloc (1 line at boot) and crash-scene logs at INFO

### P9: Boot Diagnostics
- [core_api.cpp](src/core_api.cpp): When `pcsx5_init` fails, prints a diagnostic summary:
  - Config/crash/title ID state
  - GPU headless mode status
  - Vulkan DLL availability
  - Subsystem failure error
  - Targeted suggestions per subsystem
- Load failure gets file-exists check and suggestion about retail vs fPKG

## HLE Coverage Assessment (verified adequate)
- **libSceVideoOut**: Full flip/queue model with vblank pump, equeue notifications, VRR support
- **libSceAgc (GnmDriver)**: PM4 packet walker, shader translation (M3), CP DMA, full register defaults
- **libSceSysmodule**: Full sysmodule ID → SPRX name table, fake bookkeeping (no real PRX loading needed)
- **libSceAudioOut**: Multiple backends (WASAPI, XAudio2, SDL)
- **libSceSystemService**: Real implementation with canned params
- **libSceFiber**: Win32 fiber wrappers (CreateFiber/SwitchToFiber)
- **libSceRtc**: Real Win32 FILETIME/SYSTEMTIME clock
- **libSceNet/NetCtl**: Offline emulation (CONNECTED state, loopback IP)
- **libSceRegMgr**: Default registry values
- **libSceNotification**: No-op stubs
- **libSceAppContent**: Empty content lists

**Why**: HLE has real implementations for every system module — the modules themselves are not the boot blocker. The actual problem is that games often need specific NID handling that the auto-stub can't provide (return-0 may not be the right answer).

**How to test**: Run with `PCSX5_HEADLESS=1` and `--log-level=debug`, look for "LogStubCallOnce" warnings showing which NIDs are hit during boot.

## Files Modified
- `src/config/config.h` — added `graphics.headless` to `GraphicsConfig`
- `src/gpu/vulkan_backend.cpp` — headless mode, graceful GLFW failure, ShouldCloseWindow fix
- `src/core_api.cpp` — title ID auto-detect from param.json, boot diagnostics, load failure diagnostics
- `src/kernel/kernel.cpp` — VEH TLS logs demoted from INFO to DEBUG (~12 log sites)
- `wiki/pending.md` — created with prioritized boot-blocker backlog

## Next Sprint Items (from pending.md)
- **P3**: Sysmodule dynamic loading — verify with real game trace which NIDs are missing
- **P5**: PRX module resolution — improve default search paths
- **P10**: Guest main() resolution edge cases
- **P8**: External Lua init script support
- **P6**: In-proc mode GLFW threading
