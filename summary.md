# PCSX5 — Session Summary

## 2026-07-26 — perf fix found

### Content-load 4x faster
**Root cause**: `_Getptolower` (Dinkum CRT tolower table) was called 141,750 times per minute with `LOG_INFO` on every call — 7,000 console writes/sec.

**Fix**: `LOG_INFO` → `LOG_DEBUG` in `src/hle/libkernel.cpp:1345`.

**Before**: 13 walks in 120s (6.5/min)
**After**:  13 walks in 30s  (26/min)

### dist/ ready
All fixes staged: `I:\Personal\Windows\pcsx5\dist\pcsx5.exe`

### Git log
```
cee62af perf: _Getptolower LOG_INFO->LOG_DEBUG, 4x content-load speed
62aff2d progress: update summary with latest fixes
3c88276 fix: copy per-title configs to dist
a1566c3 fix: B1.3 intermittent boot — pool base address race
... (17 more commits today)
```
