# TASK: Auto-Stub Contract Boundary Verification

## Status
**COMPLETE**

## Objective
Verify the boundary at which `GetStubContract` evaluates `SAFE`, `UNKNOWN`, and `NEVER_SAFE` classifications, tracing the path from import to execution without changing production behavior.

## Completed Actions
1. **Trace Path**: Analyzed `LinkModule`, `ResolveAny`, and `HleDispatch`. Determined that contract classification is evaluated at *runtime* (first execution), not *link-time*.
2. **Testing API Exposed**: Safely added `RegisterStubContract` to `hle.h` to allow testing of non-default contract classes without modifying production behavior.
3. **Synthetic Tests**: Created `TestStubContracts` in `hle_stub_tests.cpp`. Verified that:
   - `SAFE` successfully returns the default policy.
   - `NEVER_SAFE` terminates the guest (`ExitGuestProcess(1)`).
   - `UNKNOWN` terminates the guest (`ExitGuestProcess(1)`).
4. **AGC Preparation**: Inspected `sceAgcSuspendPoint` to determine its current state:
   - Classification: `UNKNOWN` (unpopulated).
   - Link behavior: Resolves to an auto-stub thunk.
   - Runtime behavior: Terminates guest at RIP `0x8000e72ad`.
5. **Documentation**: Authored `AUDIT-[date]-stub-contract-boundary.md` and updated `PS5_BOOT_PIPELINE.md` to reflect runtime evaluation semantics.

## Next Step
- **sceAgcSuspendPoint contract recovery**: Proceed with part 1 of TASK 04 to reconstruct the full calling contract and semantics for `sceAgcSuspendPoint`.
