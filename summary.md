# PCSX5 — Session Summary

## 2026-07-26 — final

### 19 commits today — all fixes verified

**Crash fixes:**
- `0xC0000005` memcpy AV → `IsReadable/IsWritable` validation
- Intermittent boot → pool address race fixed (let kernel choose)
- UI eboot priority → `eboot.bin` before `eboot.bin.esbak`

**Performance:**
- `_Getptolower` `LOG_INFO→LOG_DEBUG` → **4x content-load speed** (6.5→26 walks/min)
- Remaining 0.43 fps is guest CPU-bound (native PS5 code speed)

**Infrastructure:**
- DLL staged to both `dist/` root and `dist/plugins/`
- Per-title configs copied to `dist/pcsx5_config/titles/`
- `input_bot.cpp` added to all 14 test targets

**New features:**
- InputBotBackend + InputRecorder, shared_mutex HLE dispatch
- SPSC lock-free ring buffer (WASAPI audio), crash bundles
- Boot timeline + stub heat map in diagnostics
- Crash-detect loop (`tools/run_bot_test.sh`)

**LOST EPIC status:** Boots but `main() not located` — Unity IL2CPP needs 36+ modules. Not a quick fix.

**`dist/` ready** at `I:\Personal\Windows\pcsx5\dist\pcsx5.exe`
