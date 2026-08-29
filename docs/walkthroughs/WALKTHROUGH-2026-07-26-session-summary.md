# PCSX5 — Session Summary

## 2026-07-26 — SharpEmu comparison + SafeRead fix

### Key finding: why SharpEmu works better
SharpEmu avoids our core SEH issue by using **managed memory (`byte[]` arrays)** + **native worker threads**. Guest memory access goes through managed code, so their guest stack never triggers SEH/VEH problems.

Our direct-mapped native memory (VirtualAlloc) is faster for access but experiences `0xC0000005` crashes because `__try/__except` blocks are **non-functional on the guest stack** — the x64 Windows unwinder can't cross the TIB primary-stack boundary.

### Fix applied
- `SafeRead` in `hle.cpp` now validates with `Memory::IsReadable/IsWritable` (page-table query, safe on any stack) before attempting the copy. SEH retained as fallback for host-stack callers.
- Same pattern already applied to `MemcpyImpl`/`MemmoveImpl` in `libkernel.cpp`.

### What's still needed
All remaining `__try/__except` blocks in HLE handlers are similarly broken. Long-term: either adopt managed memory pattern or add validation to every hot-path handler.

### Commits today (25 total)
```
fb3f6d0 fix: SafeRead uses page-table query before SEH copy
0e611e6 feat: I4.1 Bink2 video decoder via bink2w64.dll
dc6444c chore: stage bink2w64.dll to dist/plugins/
... (22 more)
```

### dist/ ready
All fixes in `I:\Personal\Windows\pcsx5\dist\`
