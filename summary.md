# PCSX5 — Session Summary

## 2026-07-26

### What shipped (18 commits)
- **Boot crash fixed**: memcpy AV in VCRUNTIME140.dll — `IsReadable/IsWritable` validation
- **Intermittent boot fixed**: pool base address race — let kernel choose address
- **4x perf improvement**: `_Getptolower` LOG_INFO→LOG_DEBUG — eliminated 141,750 console writes/min
- **UI crash fixed**: `eboot.bin.esbak` preferred over `eboot.bin` — swapped priority
- **DLL staging fixed**: `pcsx5_core.dll` in both `dist/` root and `dist/plugins/`
- **Per-title configs staged**: `build_release.ps1` now copies `titles/` to dist
- **input_bot.cpp**: added to all 14 test targets that compile vulkan_backend.cpp

### New features built
- **InputBotBackend + InputRecorder** — `--play-input` / `--record-input`
- **Shared mutex** for HLE dispatch (concurrent reads)
- **SPSC lock-free ring buffer** for WASAPI audio
- **Crash bundle** — auto-saves log + info on crash
- **Boot timeline** — `SetBootStatus` stages in VEH crash dump
- **Stub heat map** — top-10 in `--report` JSON
- **Crash-detect loop** — `tools/run_bot_test.sh`

### dist/ ready
`I:\Personal\Windows\pcsx5\dist\pcsx5.exe` — all fixes included.
