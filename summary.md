# PCSX5 — Session Summary

## 2026-07-26

### What shipped
- **Content-load crash (0xC0000005) fixed** — memcpy AV in VCRUNTIME140.dll. Added `Memory::IsReadable/IsWritable` validation in `MemcpyImpl/MemmoveImpl` (`libkernel.cpp`). Guest-stack safe.
- **Memory pool** — 1 GB pre-reserved at `0x4000000000`, sub-allocates for `Memory::Map(hint=0)` (`memory.cpp`). Fixed: was 4GB at `0x5000000000` which broke rendering.
- **DLL staging** — `build_release.ps1` copies `pcsx5_core.dll` to both `plugins/` and `dist/` root.
- **Crash dialog buttons** — "Copy to Clipboard" + "View Raw Logs" added to the WPF crash overlay.
- **InputBotBackend (I1.1/I1.2)** — `src/gpu/input/input_bot.{h,cpp}` implements replay-based input backend. JSON frame-keyed format. `--play-input=<path>` / `--record-input=<path>` CLI args.
- **Descriptor pool (I3.3)** — Confirmed already implemented in vk_draw.cpp.
- **Shared mutex for HLE dispatch (I3.4)** — `g_hle_mutex` changed from `std::mutex` to `std::shared_mutex`. Dispatch uses shared_lock for concurrent reads; registration uses exclusive locks.
- **Audio timing (I5.1)** — Confirmed correct via kMaxBuffersInFlight pacing.

### Modified files
| File | Change |
|------|--------|
| `src/hle/libkernel.cpp` | IsReadable/IsWritable in MemcpyImpl/MemmoveImpl |
| `src/memory/memory.cpp` | 1 GB pool at 0x4000000000 |
| `src/ui_csharp/MainWindow.xaml` | Copy + Raw Log buttons on crash dialog |
| `src/ui_csharp/MainWindow.xaml.cs` | Click handlers for both new buttons |
| `build_release.ps1` | Stage DLL to dist root |
| `src/gpu/input/input_bot.h` | InputBotBackend class + replay format |
| `src/gpu/input/input_bot.cpp` | Implementation |
| `src/core_api.h` | play_input_path / record_input_path fields |
| `src/main.cpp` | --play-input / --record-input arg parsing |
| `CMakeLists.txt` | input_bot files added to both targets |

### Remaining for next session
- **I1.3** Record/replay CLI tool (record real controller input to file)
- **I2** Find-fix-rerun pipeline
- **B2/B3** GPU/HLE gaps hit during gameplay
- **I4** Video decoders
- **I5** Audio polish
