# TASK: P0 Boot Truth Recovery

## Status
**COMPLETE**

## Objective
Remove false-success execution paths (Fast Sentinel Recovery, silent 0-returning HLE auto-stubs) to establish the real first guest failure for PPSA02929.

## Completed Actions
1. **Architectural Updates**:
   - `Fast Sentinel Recovery` hack removed from `VectoredExceptionHandler`.
   - `GetStubContract` implemented and applied strictly to `ResolveAny` and `RegisterNidDbStubs`.
2. **Verification**:
   - `hle_stub_tests.cpp` correctly proves that `ExitGuestProcess` triggers on `UNKNOWN` imports.
3. **Execution**:
   - Booted `PPSA02929` and captured the real unmasked first failure.
4. **Documentation Updates**:
   - Updated `architecture/PS5_BOOT_PIPELINE.md` to reflect strict auto-stub logic and removed Fast Sentinel Recovery.
   - Authored `docs/audits/AUDIT-2026-08-24-boot-truth-recovery.md`.

## Real Failure Discovered
The first genuine unsupported contract for PPSA02929 is `sceAgcSuspendPoint` in `libSceAgc`.

## Next Steps (Next Task)
- Triage the missing `sceAgcSuspendPoint` contract.
- Implement the contract according to the project's Truth Model and Rules (evidence before implementation, strict architecture).
