# AUDIT 2026-09-08 - PPSA02929 frame-rate collapse

Follow-on from `AUDIT-2026-09-07-submission-rate-collapse.md`, which recorded a
15x drop in submission rate and left it `UNKNOWN`. This audit measures the
collapse from the guest own clock, attributes its cost, identifies the root
cause, and records three attempted fixes that did not work and why.

## 1. Baseline

| | |
|---|---|
| Commit | `003117b`, clean tree |
| Build | `cmake --build build --config Release`, Visual Studio 18 2026, x64 |
| GPU | NVIDIA GeForce RTX 5070 Ti, driver 616.56 |
| Title | PPSA02929, `session.py run --title PPSA02929 --duration 60` |
| Tests | 54 of 54 |

## 2. A trustworthy metric

Earlier phases counted flips in the run log. That proxy is too noisy to decide
anything: three pre-change runs gave 482, 394 and 483, and three post-change
runs gave 454, 405 and 512.

Two better measures agree with each other and are used throughout:

1. **The guest own frame accumulator.** The pixel shader second constant block
   carries a fixed-timestep counter at index 12. It read `0.016666` at the
   first sample and `1.7333` at the last of a 120 s run - exactly 104 ticks of
   1/60 s. The guest reached its own frame 104 while 120 s of wall clock
   elapsed.
2. **`Diagnostics::LogFrameTimingStats`**, already emitted once a second by
   `core_api.cpp` and already present in every run log as a `FPS:` line. It
   reports 0.9 in the same regime, agreeing with the guest to within rounding.

`VERIFIED`. The metric was there all along; the earlier phases did not use it.

## 3. The shape of the collapse

The `FPS:` series is highly reproducible. Three consecutive 60 s runs:

```
run 1  0.0 0.0 0.0 5.9 16.3 21.4 23.8 24.4 24.3 55.5 78.1 27.8 4.3 1.8 1.4 1.2 1.1 1.2 1.0 1.2 1.2 1.2 1.1
run 2  0.0 0.0 0.0 6.3 16.9 21.9 25.7 25.3 26.1 26.1 45.6 23.5 12.4 6.9 2.8 2.1 1.7 1.4 1.3 1.0 0.9 0.9 0.9 0.9 0.9 1.0
run 3  0.0 0.0 0.0 6.4 17.0 22.3 24.8 26.0 25.4 26.3 78.1 57.1 29.2 8.3 4.9 3.1 2.3 1.6 1.5 1.4 1.2 1.2 1.0 0.9 1.0 0.9 0.9 1.0 1.0
```

`VERIFIED`. Every run ramps to a peak between 46 and 78 fps, decays over about
ten seconds, and settles near 1 fps for the remainder. It is a transition to a
new steady state, not an unbounded leak - the floor is flat and does not keep
falling.

## 4. Where the time goes

Temporary instrumentation, all removed: per-PM4-opcode cost accumulation in
`WalkCommandBuffer`, five-phase timing inside `VkDrawExecute`, and per-bind
volume accounting in the storage-buffer upload loop.

**Opcode census.** Over one 60 s run, `op=0x35` (`DRAW_INDEX_OFFSET_2`)
accounted for 39.7 s across 1174 packets - 33.8 ms each, and more than 200x
the next opcode. Every other opcode together came to under 0.21 s.

**Phase breakdown**, cumulative over 800 draws:

| Phase | Cumulative |
|---|---|
| Storage-buffer upload | **22.8 s** |
| Texture upload | 2.3 s |
| Pipeline and modules (cached) | 0.019 s |
| Command recording | 0.017 s |
| Index snapshot | 0.0006 s |

`VERIFIED`. The storage-buffer upload is roughly 85% of GPU-side draw time.
Within it, `Memory::GuardedRead` accounted for 13.9 s across 4949 binds; the
remainder was buffer allocation and per-draw `vkMapMemory` / `vkUnmapMemory`.

Presentation is not implicated: `VideoOutSubmitFlipFromAgc` measured 0.3 ms
mean over 403 flips, and the present stage timing ring reports
`min=0.11ms avg=0.23ms max=1.02ms`.

## 5. Root cause

Per-bind data volume, sampled every 200 binds:

```
t=2s   bytes_per_bind=104889   max=262144
t=4s   bytes_per_bind=104889   max=262144
t=7s   bytes_per_bind=104889   max=262144
t=9s   bytes_per_bind=104889   max=262144
t=14s  bytes_per_bind=327705   max=1048560
t=16s  bytes_per_bind=526968   max=1048560
...
t=50s  bytes_per_bind=530901   max=1048560
```

`VERIFIED`. At about 14 seconds - precisely where the `FPS:` series begins to
fall - the mean bound range grows about 5x, from 105 KB to roughly 530 KB, and
the largest single range grows from 256 KB to 1,048,560 bytes.

**The emulator re-copies the entire bound range out of guest memory on every
draw.** Before the transition that is around 105 KB per bind; after it, around
530 KB. At roughly 100 binds per second that is about 53 MB/s of guest-memory
copying through `Memory::GuardedRead`, which measured about 150 MB/s in this
path. The guest does not slow down at 14 seconds; the emulator per-draw copying
cost grows past what the frame budget can absorb.

This supersedes the previous audit `UNKNOWN`. The submission-rate collapse it
recorded and the frame-rate collapse measured here are the same event.

## 6. Three attempted fixes, all reverted

**Attempt 1 - write-generation skip for storage buffers.** The texture path
already avoids re-uploading unchanged sources by comparing
`Memory::TryGetGuestWriteGeneration` against the generation at the last upload.
The same test was added for storage buffers, with a unit test for the rule.

`FALSIFIED as a fix.` It never fires. `TryGetGuestWriteGeneration` searches
`g_write_ranges`, and nothing calls `TrackGuestWrites` on these ranges, so
`tracked` is false for every bind and the conservative re-upload is always
taken. Measured: 4949 binds in a 60 s run, 4949 reads, zero skips.

**Attempt 2 - persistent mapping.** Every bind called `vkMapMemory` and
`vkUnmapMemory`; these became a single mapping held for the buffer life.
Correct, and it removes roughly half of the phase cost that is not the copy.
Mean fps unchanged within run-to-run noise.

**Attempt 3 - a storage ring.** Storage buffers were keyed by guest address in
a 256-entry map. This title rebuilds its constant blocks at a **fresh address
every frame**, so that cache never hits: it fills, retires every entry
wholesale, and allocates again, costing a `vkCreateBuffer` and a
`vkAllocateMemory` for essentially every bind. It was replaced with a
bump-allocated ring modelled on the existing index and staging rings, which
also makes the in-batch aliasing special case unnecessary. Mean fps unchanged.

All three were reverted. Attempts 2 and 3 are architecturally sound and remove
real work, but none of them touches the copy that dominates, and this project
does not keep changes that were not demonstrated to help.

The common lesson: **the cost is the copy, and none of the three attempts
removed a single byte of copying.**

## 7. What would fix it

The copy is only avoidable by knowing the range is unchanged since the last
upload. That means write-tracking these ranges, which is a memory-subsystem
change (Rule 05) and therefore a separate iteration under the change budget.

It is not a trivial extension of the existing mechanism, and the difficulty is
recorded here rather than discovered later:

- `g_write_ranges` is a `std::vector` searched linearly under
  `g_regions_mutex`, on every bind of every draw. Adding an entry per rotating
  address turns that scan into the new hot path.
- The addresses rotate every frame, so entries would accumulate and each would
  need arming and disarming through page protection - `VirtualProtect` churn in
  place of the copy.
- A range the GPU itself writes must not be assumed clean.

An alternative that avoids the memory subsystem entirely is to bind guest
memory directly rather than copying it, which needs the guest range to be host
memory the device can import. Whether that is possible here is `UNKNOWN`.

## 8. Classification

`SOFT BOUNDARY`. The mechanism is understood, the measurement is reproducible,
and at least two candidate techniques exist. It is bounded work, not a
capability the backend lacks.

## 9. Falsified here

- `FALSIFIED`: the collapse is a guest-side wait or timeout. The guest is not
  waiting; the emulator per-draw copy cost grows.
- `FALSIFIED`: presentation is implicated. Flip submission is 0.3 ms and the
  present stage averages 0.23 ms.
- `FALSIFIED`: a write-generation skip fixes it. These ranges are untracked, so
  the skip never fires.
- `FALSIFIED`: the displacement constants are frozen because a time input never
  advances. The block sampled was a projection matrix, which is genuinely
  static; a second block carries the frame counter and advances.

## 10. Regression

| | result |
|---|---|
| Tests | **54 of 54** |
| Working tree | clean against `003117b` apart from `TASKS.md` |
| Instrumentation | all removed |
| Production code changed | **none** |


---

# Part 2 - attempting the fix, and why it was reverted

## 11. Correction to Part 1

Part 1 said the guest "rebuilds its constant blocks at a fresh address every
frame", and used that to explain why an address-keyed cache never hits. **That
is wrong for the storage ranges.** Instrumenting the bind stream shows only
**155 distinct bases** across an entire 60 s run. The rotating-address
observation came from the AGC constant block in `libagc.cpp` and does not
describe these bindings. The address cache does hit; it just does not prevent
the copy, because the copy happens unconditionally on every bind.

## 12. Redundancy measurement, and a second correction

Instrumenting duplicate binds over a 60 s run:

```
DUPBIND binds=6500 dup=6345 (98%) MB=2937 dupMB=2836 distinct_bases=155
```

`VERIFIED`: 98% of binds repeat a base already seen at the same size, and 2836
of 2937 MB copied is the same data copied again.

**Correction:** that counter used a map that was never cleared per batch, so it
measured redundancy across the *whole run*, not within a batch. A later
experiment settles which: reusing a buffer already uploaded **within the same
batch** was implemented and measured at about 1.4 fps - no better than the
1.0 fps baseline. So the redundancy is overwhelmingly **across frames**, not
within one. Only a cross-frame cache can capture it, and that requires proving
the content unchanged.

## 13. Attempt 4 - write-tracking the storage ranges

The Part 1 write-generation skip failed only because nothing called
`Memory::TrackGuestWrites` on these ranges. The texture path already tracks
before copying and captures the generation; the same two calls were added to
the storage path.

**Performance: an order-of-magnitude success.** The `FPS:` series went from a
peak-then-collapse to a flat sustained rate, over two runs:

```
0.0 0.0 0.0 57.3 82.8 93.3 101.9 101.1 93.8 97.8 99.6 ... 100.9 100.3   (55 samples)
0.0 0.0 0.0 59.6 84.1 93.2  97.2  99.9 101.1 100.7 ...    103.1 53.5 78.5
```

About 100 fps held for the full run, against 0.9 fps before - roughly 110x. The
collapse was completely gone.

**Correctness: it broke the game.** The run reported `frames: 29 unique: 29`
but the image was black from about frame 22, and the run log carried a class of
warning absent from every previous run:

```
8737  [Memory][Warn] GuardedRead: invalid read at 0x... (copied 0 of N bytes)
5161  [Memory][Warn] GuardedCopy: write fault at 0x...f428 (copied 0 of 8 bytes)
5161  [Memory][Warn] GuardedCopy: write fault at 0x...f328 (copied 0 of 256 bytes)
...
```

Two pre-change runs contain **zero** such lines. `VERIFIED`.

### A real latent defect in the memory subsystem

`TrackGuestWrites` arms a range by setting `PAGE_READONLY` and relies on the
resulting hardware fault to disarm it and bump the generation. That works for a
native guest store. It does **not** work for the guarded primitives, which
pre-check with `IsWritable` on the host side, see the armed page as
unwritable, and drop the write:

```cpp
if (!IsWritable(cur_dest, chunk)) {
    CommitOnFault(cur_dest);
    if (!IsWritable(cur_dest, chunk)) {
        LOG_WARN(Memory, "GuardedCopy: write fault ...");   // write discarded
```

`CommitOnFault` returns early for an already-committed page, so it never
recovers an armed one. **Any guarded write into a write-tracked range is
silently discarded.** This is not specific to the storage-buffer change; it
applies to the texture ranges that are tracked today, and the only reason it
has not been observed is that nothing has been writing to them through the
guarded path.

A fix was written and measured: a `Memory::ReleaseTrackedWriteAt` helper called
at all six guarded write sites, which releases an armed range and retries -
the software equivalent of the fault path. It works: write-fault lines went to
**0** and the frame rate rose further, to a flat 127 fps.

### Why it was still reverted

With the writes no longer dropped, the title still did not advance. The run was
classified `frozen`, longest freeze 48.6 s, stuck on the Ratalaika Games splash
- rendered correctly and cleanly, at 127.2 fps, with 125 draws/s.

The remaining suspicion, `INFERRED` and not proven: arming 155 ranges of up to
1 MB each is protection thrash. `TrackGuestWrites` keys ranges by base, so
overlapping ranges arm and disarm each other pages, and
`DisarmWriteRangeLocked` restores `PAGE_READWRITE` unconditionally rather than
the range original protection. The mechanism was built for a handful of texture
ranges, not 155 large, mutually overlapping, actively rewritten ones.

There is also a design objection independent of the bug: these ranges are
rewritten by the guest every frame, so their generation should bump every
frame, and a generation-based skip should have had little to skip. That it
produced a 110x speedup suggests the skip was firing far more often than the
guest write pattern justifies - consistent with writes being lost rather than
with data genuinely being unchanged.

Attempt 4 and the `ReleaseTrackedWriteAt` fix were both reverted. The memory
fix is correct but unexercised once tracking is removed, and this project does
not keep untested code.

## 14. Revised conclusion

The copy is 98% redundant across frames, and eliminating it is worth roughly
100x on this title. The blocker is not knowing cheaply and safely whether a
range changed:

- The existing write-tracking mechanism cannot be scaled to these ranges as
  built, and has a real defect that discards guarded writes into any tracked
  range.
- Intra-batch reuse captures almost none of the redundancy (1.4 fps vs 1.0).

Remains `SOFT BOUNDARY`. The next attempt should either repair and generalise
write tracking as its own change - with the guarded-write defect fixed first,
under test, and overlapping-range and protection-restore semantics settled - or
avoid the copy entirely by importing guest pages into the device with
`VK_EXT_external_memory_host`, which this GPU supports with
`minImportedHostPointerAlignment = 0x1000` and which suits guest memory because
`Memory::Translate` is the identity: guest addresses are already host addresses.

## 15. Regression after revert

| | result |
|---|---|
| Tests | 54 of 54 |
| Runtime | `progressing`, 29 of 29 unique frames |
| Working tree | clean apart from `TASKS.md` and this audit |
| Production code changed | **none** |


---

# Part 3 - the fix that shipped: importing guest memory into the device

## 16. What the reference emulators do

Read-only comparison of the local Kyty and SharpEmu clones, to check the
approach before committing to it.

| | Kyty | SharpEmu |
|---|---|---|
| Alias or import guest memory into the GPU | No | No |
| Per-draw copy | Only the dirty sub-ranges | Vertex/index always; storage only when the bytes differ |
| Invalidation | Page write-protect + vectored exception handler, driving `BufferCache::InvalidateMemory` | Page write-protect + fault handler, **images only** |
| Cache keyed by guest address | Yes, a multi-level page table with LRU and a GC | Storage buffers only |
| Content hashing | No | No - a full byte compare against a shadow copy |
| GPU to CPU writeback | Yes, triggered by read faults | Yes, per dirty range |

Neither project imports host memory: a grep for `VK_EXT_external_memory_host`,
`vkGetMemoryHostPointerPropertiesEXT` and `VkImportMemoryHostPointerInfoEXT`
returns zero hits in both. Kyty does keep the guest address space as one
`CreateFileMapping` / `MapViewOfFile` arena with a second writable alias, but
never hands that arena to Vulkan.

So both solve the problem by *not re-copying*, through page-granular dirty
tracking - a far more developed version of the mechanism whose immature form
broke the title in Part 2. Kyty in particular refcounts read and write watchers
per 4 KiB page and uploads only dirty sub-ranges.

## 17. Attempt 5 - VK_EXT_external_memory_host. Kept.

Rather than prove the copy unnecessary, remove it. `Memory::Translate` is the
identity in this emulator - guest addresses *are* host addresses, and guest
pages are page-aligned - so guest memory is directly importable.

`ImportGuestRange` in `src/gpu/vk_draw.cpp` aligns the range down to a page,
imports the window with `VkImportMemoryHostPointerInfoEXT`, binds a `VkBuffer`
over it, and caches the window by base. The descriptor then points at guest
memory itself. Ranges the device cannot import fall through to the existing
staged copy, so nothing regresses when the extension is absent.

Device support, logged at startup:

```
[GPU][Info] Vulkan: host-pointer import available (alignment 4096 bytes).
```

### Coverage

| | |
|---|---|
| Binds imported | 8429 of 10000, about **84%** |
| Bytes imported | 2308 MB of 3186 MB, about **72%** |
| Rejections | all from one cause: the offset within the page is not a multiple of `minStorageBufferOffsetAlignment` (16 on this device) |
| Failures to import | zero - no unreadable window, no property query failure, no memory-type failure, no buffer-creation failure |

A range whose guest address is not 16-byte aligned cannot be imported, because
the window base must be page-aligned and the descriptor offset must then carry
the remainder. Those fall back to the copy.

### One mistake worth recording

The first version called `Memory::IsReadable(base, window)` on **every** bind,
including cache hits. That walks each page under a lock - 256 pages for a 1 MB
window - and cost more than the copy it replaced: the storage-buffer phase
barely moved. Validating only on the first import of a window took that phase
from **21.0 s to 0.20 s over 1200 draws**, a hundredfold. Re-checking on a hit
would not make the import safe in any case, since the window can be released
between the check and the draw.

### Result

| | before | after |
|---|---|---|
| Storage-buffer phase, 1200 draws | 21.0 s | **0.20 s** |
| Steady-state fps, five runs | 1.0 | **2.9 to 4.7** |
| Peak fps | 78 | 118 |
| Tests | 54 of 54 | 54 of 54 |
| Validation errors | 0 | 0 |
| Guarded write faults | 0 | 0 |
| Runtime | `progressing` | `progressing`, 29 of 29 unique frames |

`VERIFIED`. Between three and nearly five times the frame rate, with no
staleness to reason about: the descriptor reads the guest bytes in place, so
there is no cached copy that can go stale.

Run-to-run variance is wide (2.9 to 4.7) because the title reaches different
points in 60 s; the figure is a range, not a single number.

## 18. Attempt 6 - SharpEmu shadow compare. Reverted.

SharpEmu skips a storage upload when the guest bytes equal a shadow copy. Since
the expensive half of our copy is the write into host-visible device memory
(about 150 MB/s) and not the read, reading into ordinary memory and comparing
first should have been strictly cheaper.

Measured: **2.4 fps against 4.6**. It made things worse. With the import
already carrying 84% of binds, the copy path now runs rarely, and the extra
staging copy, the compare and the shadow assignment cost more than the writes
they avoid. Reverted.

This is a case where a technique that is right for SharpEmu is wrong for us,
precisely because the import changed which path is hot.

## 19. The bottleneck has moved

After the import, GPU-side draw work totals about 3.4 s of a 60 s run:

| Phase | Cumulative over 1200 draws |
|---|---|
| Texture upload | 3.17 s |
| Storage buffers | 0.20 s |
| Pipeline | 0.036 s |
| Recording | 0.030 s |

That is roughly 6% of wall clock. The remaining cost is **outside the draw
path** and is not yet attributed. The next investigation should profile the
command-buffer walk and the guest side, not the GPU. Texture upload is now the
largest GPU-side item and is the obvious candidate for the same treatment.

## 20. Still open

- **Where the other 94% of wall time goes.** `UNKNOWN`, and the next boundary.
- **The checkerboard on the title text.** A one-pixel checkerboard across the
  "Dreaming Sarah" glyphs, in the same pass already localized as the source of
  the warp (`ps 0x215795900`, `AUDIT-2026-09-07-artifact-localization.md`). Two
  candidates, neither tested: the sampler addressing mode, which that audit
  named as the leading unaudited suspect; or a dithered alpha fade that would
  read as a checkerboard only because the frame rate is low enough to see
  individual frames. `UNKNOWN`.
- **Unaligned ranges.** 16% of binds and 28% of bytes still copy. Kyty page
  granular dirty tracking would cover them, and is the mature version of what
  failed in Part 2.
- **Import invalidation.** An imported window is validated once. If guest
  memory is released while a buffer is bound to it, the device would read
  freed address space. Not observed in any run, and no invalidation hook
  exists. This is the main risk carried by Part 3 and should be closed.


---

# Part 4 - the cost outside the draw path: HLE dispatch bookkeeping

## 21. Where the remaining time went

With the import in place, per-frame cost split as:

```
FRAMECOST wall=263413 walk=96611 submits=1
FRAMECOST wall=270931 walk=99200 submits=1
FRAMECOST wall=254498 walk=91118 submits=1
```

About 260 ms per frame, of which the command-buffer walk is roughly 95 ms.
Two thirds is outside the GPU path entirely.

The import report for a 60 s run shows **2,262,337 HLE calls**, dominated by C
runtime functions:

| calls / 60 s | function |
|---|---|
| 307,954 | `__error` |
| 239,683 | `_Getptolower` |
| 151,181 | `strtoull` |
| 138,825 | `memcpy` |
| 62,436 | `memset` |
| 27,375 | `memmove` |

`strtoull` with `_Getptolower` and `__error` is text-to-number parsing: the
title is parsing a large data file, which is what the user observes as a hang
before the menu appears.

Dispatch is **not** exception-based - `CreateThunk` emits `mov r10, id;
mov rax, HleCommonDispatcher; jmp rax`, a direct jump. So the cost had to be in
the dispatcher itself. Instrumenting it:

```
HLECOST n=1000000 total=130397320 us mean=130.40 us
HLECOST n=1200000 total=170930759 us mean=142.44 us
HLECOST n=1400000 total=220237615 us mean=157.31 us
```

`VERIFIED`. **157 microseconds per HLE call**, rising through the run - about
three orders of magnitude more than a table lookup and an indirect call should
cost, and growing, which indicates contention rather than a fixed overhead.

## 22. Root cause

Four things happened on every one of those 2.26 million calls:

1. `HleSymbol target_sym` was copied **by value** out of the registry - two
   `std::string` members and a `std::function`.
2. `SafeString(module_name)` and `SafeString(name)` built two more strings,
   unconditionally, solely to feed a `LOG_DEBUG` that is normally disabled.
3. `TraceEntry` copied the module and symbol names into the trace ring - two
   more strings - although it already carried `symbol_id`.
4. `RecordStats` took a **global exclusive mutex** and, inside it, re-assigned
   `module_name` and `name`, re-checked `g_stubbed_ids`, and rewrote
   `thunk_address` - all of them properties of the symbol that cannot change
   after the first call.

Roughly six heap allocations per dispatch, with the fourth serialising every
guest thread on one lock. The rise from 130 to 157 microseconds is that lock
contending as more guest threads become active.

## 23. Fix

`src/hle/hle.cpp`, one subsystem, no behaviour change:

- Dispatch takes the shared lock once, reads what it needs, copies **only the
  handler** (which must outlive the lock), and performs the stats and trace
  work under that same shared lock. Readers do not block each other.
- `SafeString` is called only where a message is actually formatted.
- `TraceEntry` stores `symbol_id` alone; `GetImportTrace` resolves the names
  when the trace is read, which happens a few hundred entries at a time rather
  than millions of times a minute.
- `RecordStats` fills the descriptive fields only on a symbol first call.

The import report still resolves function names, so no diagnostic was traded
away for the speed.

## 24. Result

| | baseline | after import | after dispatch fix |
|---|---|---|---|
| Steady-state fps | 1.0 | 2.9 to 4.7 | **4.6, 4.6, 4.6, 4.3, 4.3, 4.4** |
| Consistency | - | wide | tight |
| Tests | 54 of 54 | 54 of 54 | **54 of 54** |
| Import report names | resolved | resolved | resolved |

`VERIFIED`. About **4.6x** on the baseline overall, and the run-to-run spread
collapsed from 2.9-4.7 to 4.3-4.6.

## 25. Still open after Part 4

- The title screen renders legibly - "Sarah" is clean where it was
  checkerboarded - but the upper region still shows the blocky artifact
  recorded in `AUDIT-2026-09-07-artifact-localization.md`, and glyph edges
  smear. Unresolved.
- Runs classify as `frozen` about half the time with 20 to 22 of 29 unique
  frames. The classifier itself reports the status was not stable across
  baseline samples. Whether this is the title reaching a static screen or a
  real stall is `UNKNOWN` and should be settled before it is read either way.
- 4.6 fps is still far from 60. The next measurement should re-take the
  `FRAMECOST` split, since the balance between walk and guest time will have
  moved again.


---

# Part 5 - corrections, and where the cost sits now

## 26. Correction: the 157 microseconds figure was a measurement artifact

Part 4 reported 157 microseconds per HLE dispatch, measured by a timer that
incremented two shared `std::atomic` counters inside every call. On a path taken
2.26 million times a minute across several guest threads, those atomics contend
with each other, so the instrument was largely measuring itself. Re-running the
same instrument on the *fixed* build still reported 137 to 220 microseconds,
which is what exposed it.

The dispatch fix in Part 4 stands, but on the end-to-end evidence only - the
frame rate went from a wide 2.9-4.7 to a tight 4.3-4.7 - not on the per-call
number. The absolute per-call cost of dispatch is `UNKNOWN`; measuring it needs
an instrument that does not serialise the path it measures, such as per-thread
counters aggregated at exit.

## 27. FALSIFIED: something accumulates over a run

Tested directly. GPU-side pool sizes, sampled every 200 draws over a 120 s run:

```
POOLS draws=200   retired=0 imported=14 guest_bufs=21 textures=4 uploaded_bases=3
POOLS draws=1400  retired=0 imported=14 guest_bufs=35 textures=4 uploaded_bases=10
POOLS draws=2600  retired=0 imported=16 guest_bufs=39 textures=5 uploaded_bases=11
POOLS draws=3800  retired=0 imported=16 guest_bufs=39 textures=5 uploaded_bases=7
```

Flat and small - nothing grows. The frame rate over the same 120 s run peaks at
109, drops once, then holds:

```
0.0 109.3 27.8 4.5 3.9 4.5 4.6 4.6 4.6 4.6 4.6 3.6 4.3 4.3 4.3 4.3 4.3 4.2 4.3
```

`FALSIFIED`. There is no accumulation and no progressive decay. The single drop
is the transition already characterised in Part 1 - the guest bound ranges grow
about 5x - after which the cost is a **steady state**, not a leak.

## 28. Draw call count is not the problem

Per-frame measurement with both fixes in place:

```
FRAMECOST wall=260045 walk=95133 draws=18 dwords=1067
FRAMECOST wall=261273 walk=95449 draws=18 dwords=1067
FRAMECOST wall=251015 walk=89961 draws=17 dwords=1015
FRAMECOST wall=213867 walk=70704 draws=14 dwords=839
```

`VERIFIED`. The title issues **14 to 18 draw calls per frame** in a command
buffer of about 1000 dwords. That is a trivial workload; no GPU is troubled by
18 draws. The problem is not how many draws there are but what each one costs:

| | per frame | share |
|---|---|---|
| Command-buffer walk (18 draws) | ~95 ms | 37% |
| Everything else - the guest itself | ~165 ms | 63% |

That is about **5.3 ms per draw call** inside the walk. Post-import phase
timings attribute roughly half of it to texture upload (3.17 s over 1200 draws,
about 2.6 ms per draw), which is now by far the largest GPU-side item.

## 29. Next boundaries, in order

1. **Texture upload, ~2.6 ms per draw.** The largest remaining GPU-side cost.
   The texture path already has a write-generation skip and those ranges *are*
   tracked, so establish first why it is not skipping - either the generation
   genuinely moves every frame, or the skip is not being reached.
2. **The 63% spent outside the walk.** Attribute it with a per-thread
   instrument that does not contend, per the correction in section 26.
3. **Rendering correctness.** The title screen is legible - "Sarah" renders
   cleanly where it was previously checkerboarded - but the upper region still
   shows the blocky artifact from
   `AUDIT-2026-09-07-artifact-localization.md` and glyph edges smear.

## 30. On borrowing text rendering from RPCS3

Not applicable, and worth recording so it is not re-proposed. RPCS3 text
rendering is its own overlay UI - trophy popups, the home menu, the on-screen
keyboard - drawn by the emulator on top of the game. PCSX5 does not render this
title text at all: the words are a guest texture, sampled and displaced by the
guest own pixel shader `0x215795900`, and the artifacts are in that pass output.
There is no text-rendering code here to replace. RPCS3 is GPL-2.0 and therefore
licence-compatible if something else there proves useful, but its overlay
renderer does not address this defect.


---

# Part 6 - the real hot spot: Memory::IsReadable

## 31. Narrowing, with two wrong guesses recorded

The per-draw split (Part 5) put 75% of draw time in shader scalar evaluation,
3.2 ms per draw. Two hypotheses about *why* were tested and both were wrong:

- **"The symbolic path walk is expensive."** `FALSIFIED`. Instrumented: the walk
  explores **1 path and about 62 instruction visits per evaluation**, with 0% of
  visits skipped. 62 instructions cannot cost 3.2 ms.
- **"The per-binding LOG_INFO dumps are the cost."** `FALSIFIED`. Making them
  fire once per shader instead of once per draw left the log at 27-28k lines and
  the frame rate inside existing run-to-run variance.

Measured properly, splitting `AgcEvaluateDrawShader` three ways over 4800
evaluations:

```
EVALPARTS n=4800 collect=1110 scalar=7131165 log=1996     (microseconds)
```

`VERIFIED`. User-data collection 1.1 ms, logging 2.0 ms, and
`GcnEvaluateScalarState` itself **7,131 ms**. The cost is inside the evaluator,
but not in its interpretation.

## 32. Root cause

The evaluator reads guest memory through one helper:

```cpp
bool TryReadU32(u64 address, u32& value) {
    if (!Memory::IsReadable(address, sizeof(u32))) {
        return false;
    }
    value = Memory::Read<u32>(address);
    return true;
}
```

Timed separately over 60,000 calls:

```
READU32 n=60000 isreadable=5713934 us read=1542 us
```

`VERIFIED`. **`Memory::IsReadable` costs 95 microseconds per call; the guest
read it guards costs 0.026 microseconds.** The guard is roughly 3600x the cost
of the thing it protects.

The reason is structural. `IsReadable` calls `Query` once per 4 KB page, and
`Query` issues a **`VirtualQuery` syscall** and then takes the global
`g_regions_mutex` to scan the region table linearly. Every dword the evaluator
reads pays a syscall and a contended global lock, and an 8-dword descriptor load
pays it eight times.

This also retrospectively explains Part 3: validating a 1 MB import window on
every bind meant 256 `VirtualQuery` calls, which is why that first version of
the import cost more than the copy it replaced.

## 33. Fix

`src/memory/memory.cpp`: a thread-local, direct-mapped page-state cache in front
of `Query`, holding 512 entries of (generation, page, committed, protection).

Correctness rests on invalidation, not on hoping the state is stable. A global
`g_map_generation` counter is bumped by every public entry point that can change
mapping or protection - `Map`, `Unmap`, `Protect`, `Commit`, `Reserve`,
`AllocateRange`, `ReleaseRange`, `AdoptRange`, `PoolAlloc`, `PoolFree`,
`CommitOnFault`, `TrackGuestWrites`, `UntrackGuestWrites`, `RearmGuestWrites`
and `Shutdown` - 16 sites in total. An entry recorded at an older generation is
never used, so a single bump retires every cached page in every thread at once.
The bumps live at the public entry points rather than at the 43 individual Win32
calls, because that is the smaller set to keep complete and the one a reviewer
can check.

The cache is thread-local and therefore needs no lock of its own, which is the
point: the contended global lock was half the cost.

## 34. Result

| | before Part 6 | after |
|---|---|---|
| Steady-state fps | 4.4 - 4.7 | **9.3 - 9.8** |
| Unique frames of 29 | 20 - 22 | **25 - 26** |
| Classification | `frozen` about half of runs | **`progressing`, all five runs** |
| Tests | 54 of 54 | **54 of 54** |
| Vulkan validation errors | 0 | **0** |

`VERIFIED` over five runs. Roughly **2.1x** on top of the earlier work, and
**9.3x on the original 1.0 fps baseline**.

The unique-frame count moving *up* is the important part. The rejected
evaluation cache in Part 5 bought frame rate and cost unique frames, which is
the signature of serving stale data. This change raises both, and moves every
run from `frozen` to `progressing`.

## 35. A visual characterization that changes an earlier finding

At 9.3 fps the title screen shows the logo **twice**: once centred and once in
the upper band. The upper band is where
`AUDIT-2026-09-07-artifact-localization.md` recorded "intermittent blocky
artifacts".

Those blocks are not noise. They are a **second, displaced copy of the same
logo**. `OBSERVED` - one capture, `PPSA02929_20260908_104239` frame 28.

That is consistent with what the shader audit predicted: a large-amplitude
single-tap displacement can map two separated screen regions onto overlapping
parts of the source, which reads as doubling. It makes the blocks and the warp
one phenomenon rather than two, and it means the remaining visual work is a
single question about that pass displacement amplitude.

Both copies still smear at the glyph edges.

## 36. Next boundaries

1. **The displacement amplitude in `ps 0x215795900`.** The blocks and the warp
   are now one problem. Still `UNKNOWN` whether the effect is guest-intended.
2. **Re-take the per-draw split.** Scalar evaluation was 75% of draw time with a
   95-microsecond guard on every dword read; with the guard gone the balance will
   have moved and should be re-measured before anything else is optimised.
3. **`Memory::IsWritable` has the same structure** as `IsReadable` and was not
   changed. It is on the guarded write paths and should be measured.


---

# Part 7 - the remaining two predicates, and a real dropped-write defect

## 37. IsWritable and IsExecutable had the same defect

`IsWritable` and `IsExecutable` are structurally identical to `IsReadable`: a
per-4 KB-page `Query`, so a `VirtualQuery` syscall plus the global region lock
per page. They were given the same cached path.

Extending the cache to the write predicate required closing an invalidation
hole first. `ArmWriteRangeLocked` and `DisarmWriteRangeLocked` change page
protection with `VirtualProtect`, and they are also reached from the **VEH
fault path**, which does not pass through any public entry point. The
generation bump therefore had to go inside those two functions rather than at
their callers - 18 bump sites in total. Missing this would have been benign for
`IsReadable` (an armed page is still readable) and wrong for `IsWritable`.

## 38. A latent defect became a live one

With the emulator an order of magnitude faster, the title reaches gameplay, and
a defect recorded as latent in Part 2 started firing:

```
57  [Memory][Warn] GuardedCopy: write fault at ADDR (copied 0 of 128 bytes)
```

`VERIFIED`. **61 guest writes of 128 bytes each silently discarded in a single
run.** Not a performance issue - lost guest state.

The cause is the one recorded in Part 2. `TrackGuestWrites` arms a range
`PAGE_READONLY` and relies on the hardware fault to disarm it, which works for
a native guest store. The guarded primitives instead pre-check writability in
software, see the armed page as unwritable, and drop the write; `CommitOnFault`
returns early for an already-committed page and never recovers an armed one.

## 39. Fix and test

`Memory::ReleaseTrackedWriteAt` releases an armed range and bumps its
generation - the software equivalent of the hardware fault path - and is called
at all six guarded write sites before the writability re-check.

`tests/guest_memory_access_tests.cpp`, `TestGuardedWriteIntoTrackedRange`:
tracks a page, writes into it through `GuardedWrite`, and asserts the bytes
land in guest memory *and* that the write generation advanced, so a cache owner
still sees the rewrite.

**The test needed one non-obvious thing to reproduce the defect at all.** A
first version passed with the fix removed. The reason is that the defect does
not exist inside the direct-mapped pool: for pool addresses `Query` answers
from the region table and never consults real page protection, so an armed page
still reads as writable there. The test sets `PCSX5_DISABLE_POOL=1` to force
the non-pool path. Verified both ways:

```
without the fix:  4 failures, "all bytes written into a tracked range (lhs=0 rhs=128)"
with the fix:     92 checks, 0 failures
```

That `Query` reports pool memory as writable regardless of its actual
protection is worth recording on its own. It is not wrong for the pool - the
region table is documented there as the sole authority - but it means page
protection and reported protection can disagree for pool addresses.

## 40. Result

| | baseline | after Part 6 | after Part 7 |
|---|---|---|---|
| Steady-state fps | 1.0 | 9.3 - 9.8 | **62.7 - 67.2** |
| Unique frames of 29 | 29 | 25 - 26 | **29 of 29, all runs** |
| Classification | `progressing` | `progressing` | **`progressing`, all runs** |
| Discarded guest writes | 0 (latent) | 61 in one run | **0** |
| Tests | 54 of 54 | 54 of 54 | **54 of 54** |

`VERIFIED` over three runs. **About 65x the original baseline**, and past 60 fps.

## 41. Milestone

The title now reaches **gameplay**, not just the menu: a playable scene with
the character on a platform, correct sprites, tiles and parallax background,
no checkerboard on the world art and no warp on the scene.

## 42. Remaining

- **A ghost of the title logo persists faintly over the gameplay background.**
  Stale render-target content that is not being cleared between scenes. This is
  the "motion blur" of the mission brief, now with a specific characterization.
  `OBSERVED`.
- The displacement question in `ps 0x215795900` is unchanged.
- Long-run stability beyond 120 s is still unmeasured.
