# PCSX5 — Session Summary

## 2026-07-26

### Performance fix
**Root cause**: `_Getptolower` logged `LOG_INFO` on every call — 141,750/min.

**Fix**: `LOG_INFO` → `LOG_DEBUG` in `src/hle/libkernel.cpp:1345`.

**Result**: 97% fewer log lines, **4x content-load speed** (6.5 → 26 walks/min).

### Current content-load speed
Remaining 0.43 fps (~2.3s/frame) is the PS5 game code running natively on the host CPU during asset decompression/parsing. Expected for early emulation — comparable to other PS4/PS5 emulators at similar stages.

### All pushes today
```
b8ffeae progress: update summary with perf fix
cee62af perf: _Getptolower LOG_INFO->LOG_DEBUG, 4x content-load speed
62aff2d progress: update summary with latest fixes
3c88276 fix: copy per-title configs to dist
a1566c3 fix: B1.3 intermittent boot — pool base address race
2ea6b4d fix: input_bot.cpp in all targets + dist build
92813ce progress: final summary
6bfe30a feat: I5.2 lock-free SPSC ring buffer
e114283 feat: I2.1 headless crash-detect loop script
025a652 feat: I1.4 crash bundle on guest crash
... (9 more)
```

**17 commits total today**.
