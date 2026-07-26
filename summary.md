# PCSX5 — Session Summary

## 2026-07-26

### What shipped
- **Content-load crash (0xC0000005) fixed** — memcpy AV in VCRUNTIME140.dll. Added `Memory::IsReadable/IsWritable` validation in `MemcpyImpl/MemmoveImpl` (`libkernel.cpp`). Guest-stack safe.
- **Memory pool** — 1 GB pre-reserved at `0x4000000000`, sub-allocates for `Memory::Map(hint=0)` (`memory.cpp`). Fixed: was 4GB at `0x5000000000` which broke rendering (pool addresses conflicted with guest expectations).
- **DLL staging** — `build_release.ps1` copies `pcsx5_core.dll` to both `plugins/` and `dist/` root (CLI implicitly links, must find DLL at process start before `SetDllDirectoryW`).
- **Crash dialog buttons added** — "Copy to Clipboard" copies crash info + full log; "View Raw Logs" opens the full console log in a read-only window.

### dist/ (all fresh 06:15)
| File | Time | What |
|------|------|------|
| `pcsx5.exe` | 06:15 | WPF UI with crash dialog buttons |
| `pcsx5_cli.exe` | 06:13 | CLI runner |
| `pcsx5_core.dll` | 06:11 | Core DLL (fix + pool) |
| `plugins/pcsx5_core.dll` | 06:11 | Same DLL for SetDllDirectoryW |

### Modified files
| File | Change |
|------|--------|
| `src/hle/libkernel.cpp` | IsReadable/IsWritable in MemcpyImpl/MemmoveImpl |
| `src/memory/memory.cpp` | 1 GB pool at 0x4000000000 |
| `src/ui_csharp/MainWindow.xaml` | Copy to Clipboard + View Raw Logs buttons |
| `src/ui_csharp/MainWindow.xaml.cs` | Click handlers for both new buttons |
| `build_release.ps1` | Stage DLL to dist root |
