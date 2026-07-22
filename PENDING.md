# Pending — prioritized work queue

Low/medium priority first; high-priority items broken into small,
independently verifiable pieces.

## High priority — M3.3 menus (broken into pieces)

Context: splash renders correctly; after ~180 frames the guest enters a
content-load phase (no draws), then the run dies silently ~8-10 min in.

- [~] H4. Verify menus render after H1-H3; compare against SharpEmu
      golden dumps; fix remaining format/tiling/blend mismatches.
      IN PROGRESS (2026-07-22): two independent crashes identified.
      
      Crash A — Construct runtime null variants (tag=0xb0):
        Applied fix: zero user data in HeapAllocLocked (liblibc.cpp:97-100,
        109-111) to match Orbis OS zero-initialized heap expectations.
        Hypothesis: Construct runtime's internal allocator reuses freed
        blocks without clearing; stale 0xb0 poison bytes from freed blocks
        appear as null variants during JSON parsing.  Not yet verified.
      
      Crash B — VCRUNTIME140.dll access violation (0xC0000005 at +0x1cc54):
        Occurs during AGC texture ops (d-6uF9sZDIU + memcpy chain).  VEH
        catches it (logs + CONTINUE_SEARCH), game survives hundreds more
        frames.  The guest memory scan finds leaked host pointers (0x7ff...)
        in guest memory — likely root cause.  Eventually one crash becomes
        a C++ exception with no handler → std::terminate.
        Not yet diagnosed: which HLE function writes host pointers into
        guest memory.  Requires targeted logging of AGC map/write paths.
      
      Both blockers remain open.  Diagnostics in src/hle/liblibc.cpp:
      throw-type/message dump, failing-variant tag dump, parser-frame
      and array-element dumps, .work/guest_text.bin snapshot.



## From KytyPS5 & SharpEmu commit analysis (2026-07-22)

Reviewed 50+ KytyPS5 commits and 414 SharpEmu commits for patterns/features
not yet ported.  Sorted by estimated impact on game booting.

### HIGH — missing kernel primitives (may block game boot)

- [x] P1. **Implement sceKernelSyncOnAddressWait/Wake** (SharpEmu #422)
      - [x] P1.1 Define wait/wake structures & state tables in `src/hle/libkernel_sync.cpp` or new `src/hle/libkernel_syncaddr.cpp`.
      - [x] P1.2 Implement `sceKernelSyncOnAddressWait` (sleep on address until wake or timeout).
      - [x] P1.3 Implement `sceKernelSyncOnAddressWake` (wake N waiting threads for address).
      - [x] P1.4 Register NID export symbols in HLE dispatcher (`libkernel`).
      - [x] P1.5 Unit tests for single & multi-threaded address wait/wake in `kernel_sync_tests.cpp`.

- [x] P2. **Full SysV variadic float ABI (XMM0-XMM7 capture)** (SharpEmu #59)
      - [x] P2.1 Extend `src/hle/dispatcher.asm` trampoline to save XMM1-XMM7 registers to guest stack/args struct on import calls.
      - [x] P2.2 Update `GuestArgs` / `HleDispatch` parameter parser to expose floating point register bank.
      - [x] P2.3 Update `src/hle/guest_printf.cpp` vsnprintf format parser to extract float args from XMM bank.
      - [x] P2.4 Unit test variadic float formatting with multiple `%f` / `%g` arguments in `guest_printf_tests.cpp`.

- [ ] P3. **BMI1/BMI2/ABM instruction emulation** (SharpEmu #249)
      - [ ] P3.1 Add CPU feature detection & fallback handlers in `src/cpu/amd_compat.cpp` for BMI1/BMI2/ABM instructions.
      - [ ] P3.2 Implement decoding & execution for bitwise instructions (`ANDN`, `BLSI`, `BLSMSK`, `BLSR`, `BZHI`, `BEXTR`).
      - [ ] P3.3 Implement decoding & execution for bit manipulation / count instructions (`TZCNT`, `LZCNT`, `RORX`, `SARX`, `SHLX`, `SHRX`, `PDEP`, `PEXT`).
      - [ ] P3.4 Unit tests in `sse4a_bitfield_tests.cpp` for BMI1/BMI2 instruction fallbacks.

- [ ] P4. **TLS reservation audit: StartupStaticTlsReservation** (SharpEmu #454)
      - [ ] P4.1 Audit `src/kernel/tls.cpp` static TLS headroom (increase from 128KB to matched target).
      - [ ] P4.2 Check main thread TLS allocation size vs game binary requirement (`StartupStaticTlsReservation`).
      - [ ] P4.3 Register/verify NID `BHouLQzh0X0` stub / export handling.

### MEDIUM — system completeness

- [ ] P5. **Implement pthread semaphore exports** (SharpEmu #424)
      - [ ] P5.1 Define `sem_t` object structure in `src/hle/libkernel_sync.cpp`.
      - [ ] P5.2 Implement `sem_init`, `sem_destroy`, `sem_wait`, `sem_trywait`, `sem_post`, `sem_getvalue`.
      - [ ] P5.3 Register POSIX semaphore NID exports in `libkernel`.
      - [ ] P5.4 Unit tests in `kernel_sync_tests.cpp`.

- [ ] P6. **Add RandomExports HLE** (SharpEmu #413)
      - [ ] P6.1 Implement `sceRandomGetRandomNumber` and hardware RNG fallback exports using C++ `<random>`.
      - [ ] P6.2 Register NID exports in HLE symbol table.

- [ ] P7. **Add missing libc string exports** (SharpEmu #132)
      - [ ] P7.1 Implement HLE exports for `strchr`, `strrchr`, `memchr`, `strcat`, `strncat`, `strstr` in `src/hle/liblibc.cpp`.
      - [ ] P7.2 Register missing NID symbols in `libSceLibcInternal` / `libc`.

- [ ] P8. **Implement pthread_yield NID** (SharpEmu #426)
      - [ ] P8.1 Register NID `B5GmVDKwpn0` / `pthread_yield` explicitly in `libkernel.cpp` (calling `SwitchToThread()` / `std::this_thread::yield()`).

- [ ] P9. **Fix lazy pthread object initialization** (Kyty 42d42e3)
      - [ ] P9.1 Audit `src/hle/libkernel_sync.cpp` lazy mutex/cond initialization sequence against Kyty reference fix.
      - [ ] P9.2 Verify concurrent lazy initialization under race conditions.

### LOW — features and compatibility

- [ ] P10. **Add ASTRO BOT compatibility stubs** (SharpEmu #481)
      - [ ] P10.1 Implement stubs for `ContentExport`, `Font`, and `Pad` NID calls required by Astro Bot.

- [ ] P11. **Implement sceFontGetVerticalLayout** (SharpEmu #492)
      - [ ] P11.1 Implement `sceFontGetVerticalLayout` metrics stub in `src/hle/libfont.cpp` (or font HLE).

- [ ] P12. **Memory: back free pages of partially-overlapping fixed mapping** (SharpEmu #458)
      - [ ] P12.1 Update `src/memory/memory.cpp` fixed-address reservation logic to back free pages when new reservation overlaps existing partial mapping.

- [ ] P13. **CPU: preserve guest return value across TLS lookup** (SharpEmu #104)
      - [ ] P13.1 Update TLS lookup helper in `src/cpu/cpu.cpp` / `dispatcher.asm` to preserve RAX/XMM0 guest return values across internal TLS resolution calls.
