# PCSX5 — Session Summary

## 2026-07-26

### Boot intermittentcy fixed
**Root cause**: Memory pool forced to `0x4000000000` shifted VA layout, triggering a guest-initialization race that caused ~60% of boots to hang before reaching M3 rendering.

**Fix**: Let kernel choose pool address (`VirtualAlloc(nullptr, ...)`). 10/10 runs now succeed (was 4/10). Pool stays enabled.

### Build state
- `dist/` fully staged with all fixes
- `PCSX5_DISABLE_POOL=1` env var available for debugging

### All commits (15 total)
```
a1566c3 fix: B1.3 intermittent boot — pool base address race
2ea6b4d fix: input_bot.cpp in all targets + dist build
92813ce progress: final summary
6bfe30a feat: I5.2 lock-free SPSC ring buffer
e114283 feat: I2.1 headless crash-detect loop script
025a652 feat: I1.4 crash bundle on guest crash
... (9 more commits from earlier)
```
