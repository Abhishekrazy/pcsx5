# PCSX5 — Session Summary

## 2026-07-26

### 21 commits today

**Bink2 decoder (I4.1) — new:**
- Full implementation in `src/media/bink2_decoder.cpp`
- Dynamic loading of `bink2w64.dll` via `LoadLibrary`/`GetProcAddress` (no SDK headers)
- Bink2Open/Bink2GetInfo/Bink2DecodeFrame/Bink2GetFrameData/Bink2Close
- Converts to VideoFrame (BGRA8/YUV420)
- `build_release.ps1` stages DLL to `dist/plugins/`

**Performance fix:**
- 4x content-load from `_Getptolower` LOG_INFO→LOG_DEBUG

**Infrastructure fixes:**
- DLL staging (root + plugins/), per-title configs in dist
- input_bot.cpp in all test targets
- eboot.bin priority over eboot.bin.esbak
- Pool address race → kernel-chosen address
- Bink2 GetCurrentTime macro collision with Windows SDK
