# PCSX5 — Session Summary

## 2026-07-26

### Fixed
- **Intermittent boot (B1.3)**: Pool at forced address caused ~60% boot failures. Fixed by letting kernel choose pool base.
- **Per-title configs not staged**: `build_release.ps1` now copies `pcsx5_config/titles/` to `dist/`.
- **DLL staging**: Both `dist/` root and `dist/plugins/` for implicit link.
- **input_bot.cpp link errors**: Added to all 14 targets that compile `vulkan_backend.cpp`.

### Built
- InputBotBackend + InputRecorder (--play-input / --record-input)
- Shared mutex for HLE dispatch (concurrent reads)
- Boot timeline in crash dump, stub heat map in --report JSON
- SPSC lock-free ring buffer for WASAPI audio
- Crash-detect loop script (`tools/run_bot_test.sh`)
- Crash bundle saver (I1.4)
- Copy + Raw Logs on WPF crash dialog

### dist/ ready
`I:\Personal\Windows\pcsx5\dist\pcsx5.exe` — all fixes included.

### Next session
- Performance: 0.4 fps content-load. Bottleneck unknown. Needs profiling or async asset loading.
- Remaining items in `pending.md` blocked on game progression or external SDKs.
