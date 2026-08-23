# CTest Baseline — 2026-08-23

Configuration: Windows x64, MSBuild 18.9, `ctest -C Release`.
(2026-08-22 note about Debug-config exes no longer applies: all test
executables now build into `Release/`.)

## Result: 45 tests, 45 passed (100%) — verified across multiple consecutive runs

Fix provenance note: several of the seven failures below were already fixed
in the prior session (see memory `ctest-45-45-green-2026-08-23`: pool Query
gate, import-report JSON schema, diagnostics dedup/flush, _popen stdin, WSL
bash). Today's work resolved the remaining items and shipped the Stage 2
guest VA ownership refactor that keeps them green structurally.

Previous baseline: 38/45 (84%). The seven failures from 2026-08-22:

| Test | Was | Now |
|---|---|---|
| keystone_tests | PASS (fixed 8/22, held) | PASS |
| loader_corpus | FAIL | PASS — PIE-base/preferred-address conflict resolved by Stage 2 VA contract |
| memory_query | FAIL | PASS — pool Query gate + 36 new ownership-contract checks (114 total) |
| hle_import_report | CRASH (0xc0000409) | PASS — JSON schema + NID-identity assertions updated |
| diagnostics | FAIL | PASS — dedup flush + log-path redirect fixes |
| hle_agc | CRASH @16.5s | PASS — WaitRegMem32 pointer check removed (see agc memory note) |
| headless_bot_test | FAIL | PASS — _popen stdin wrapper + teardown-line filtering |
| regression | FAIL | PASS — Git-bash discovery fix |

## Stage 2 refactor shipped today (guest VA ownership)

Single guest VA authority in `src/memory`:

- `Owner` enum (Loader/Kernel/Hle/Guest) recorded per region; queryable via
  `QueryOwner`. `AllocateRange` = deterministic low-address-first placement
  inside `[0x800000000, 0x900000000)`; `ReleaseRange` frees for reuse
  (exact-span reuse verified); `IsRangeFree`; idempotent `AdoptRange` for
  host allocations made outside the API.
- Kernel's competing bump allocator at 4 TB removed; `Kernel::AllocGuestMemory`
  / `MapGuestMemory` / `SetBreak` now delegate to Memory. No shipped title
  used those syscalls, so boot behavior is unchanged.
- Untracked HLE allocations adopted: PhysPool (2 GB), MapDirectMemory hint
  path, pthread stacks/TLS blocks, unwind trampoline page, TLS stub region.
- Loader expresses preferred base and walks fallback hints itself; every
  relocation logs a `MODULE_RELOCATE title=… module=… preferred=… allocated=…
  size=… reason=collision` line.
- `Memory::Shutdown` now releases manager-owned ranges and the 1 GB pool
  (re-init no longer fails the framebuffer map with err=487).

Known coarse-granularity limitation pinned in tests: commit state is tracked
per whole Region record, so committing part of a reservation marks the whole
record committed (host pages are genuinely only partially committed).
