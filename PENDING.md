# Pending — prioritized work queue

Low/medium priority first; high-priority items broken into small,
independently verifiable pieces.

## High priority — M3.3 menus (broken into pieces)

Context: splash renders correctly; after ~180 frames the guest enters a
content-load phase (no draws), then the run dies silently ~8-10 min in.

- [ ] H4. **Verify menus render after H1-H3; fix remaining menu blockers**
      - [ ] H4.1 **Construct runtime null variants (Crash A)**: Audit heap recycling / `HeapAllocLocked` zeroing for 0xb0 poison byte reuse during JSON parse.
      - [ ] H4.2 **VCRUNTIME140 access violation (Crash B)**: Audit AGC map/write paths (`d-6uF9sZDIU` / `memcpy` chain) for leaked host pointers (`0x7ff...`) written into guest memory.
      - [ ] H4.3 **Unwind & Exception diagnostics**: Verify VEH catch / C++ exception handler unwinding during menu transitions.
      - [ ] H4.4 **Golden Frame Comparison**: Compare output menu rendering against reference dumps; fix format, tiling, and blend mismatches.
