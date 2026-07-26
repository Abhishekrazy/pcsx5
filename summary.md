# PCSX5 — Session Summary

## 2026-07-26 (final)

### Completed this session
| Task | What | Files |
|------|------|-------|
| **B1.1/B1.2** | Memcpy AV crash fixed — `IsReadable/IsWritable` validation | `libkernel.cpp` |
| **B1.2b** | DLL staging — core DLL in both `dist/` and `dist/plugins/` | `build_release.ps1` |
| **I1.1/I1.2** | InputBotBackend — JSON replay → ControllerState | `input_bot.{h,cpp}` |
| **I1.3** | InputRecorder — live recording to NDJSON | `input_bot.{h,cpp}` |
| **I3.2** | Memory pool — 1 GB at 0x4000000000 | `memory.cpp` |
| **I3.3** | Descriptor pool — confirmed already implemented | `vk_draw.cpp` |
| **I3.4** | Shared mutex HLE dispatch — concurrent reads | `hle.cpp` |
| **I5.1** | Audio timing — confirmed correct via pacing | `libaudioout.cpp` |
| **I6.1** | Boot timeline in crash dump | `gpu.h`, `vulkan_backend.cpp`, `kernel.cpp` |
| **UI** | Copy to Clipboard + View Raw Logs on crash dialog | `MainWindow.xaml` / `.cs` |

### Remaining
| Task | Priority | Notes |
|------|----------|-------|
| I3.5 VRR frame pacing | Low | Remove fixed 60 Hz throttle when VRR enabled |
| I3.6 Shader warmup | Low | Pre-compile from disk cache during boot |
| I5.2/I5.3 Audio perf | Low | Shared ring buffer, zero-copy |
| I2.x Pipeline | Medium | Needs I1 completed first |
| B2/B3 GPU/HLE | Medium | Needs game to reach new content |
| I4.x Video decoders | Low | Needs Bink SDK / FFmpeg headers |
