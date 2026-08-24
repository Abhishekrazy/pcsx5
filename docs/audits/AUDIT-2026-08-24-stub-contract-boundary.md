# AUDIT: Auto-Stub Contract Boundary Verification

## 1. Trace The Exact Path
When the guest loads a module, it resolves imports through `LinkLoadedPrxModules`. The resolution path is:
1. `LinkModule` iterates over ELF relocations.
2. `resolve_external` attempts PRX exports, real HLE implementations, and NID-base fallback.
3. If all fail, `HLE::ResolveAny` creates an auto-stub.
4. `ResolveAny` registers a lambda under the module "unknown" in `g_symbol_registry`, creating a thunk.
5. `LinkModule` writes this thunk address into the guest GOT.
6. Guest execution continues. **Termination does not occur at link time.**
7. When the guest calls the missing import, execution jumps to the thunk, entering the `HleDispatch` host routine.
8. The thunk executes the lambda registered by `ResolveAny`.
9. The lambda evaluates `GetStubContract`.
10. If the contract classification is `UNKNOWN` or `NEVER_SAFE`, `ExitGuestProcess(1)` is triggered with a fatal log.

## 2. Link-Time vs Runtime Semantics
- **SAFE**: Link-time succeeds. Runtime successfully evaluates the contract, logs a warning, and returns the `default_return_policy` without terminating the guest.
- **UNKNOWN**: Link-time succeeds. Runtime evaluates the contract, logs a fatal error detailing the symbol/RIP/RSP, and calls `ExitGuestProcess(1)`.
- **NEVER_SAFE**: Link-time succeeds. Runtime evaluates the contract, logs a fatal error, and calls `ExitGuestProcess(1)`.

The chosen architecture evaluates contracts at *first execution* rather than *link time* because missing imports might never be executed (e.g., error handling, untouched subsystems). Terminating at link time would break games unnecessarily.

## 3. Test With a Synthetic Import
Since the codebase currently leaves `g_stub_contracts` unpopulated (causing all imports to be `UNKNOWN`), an API `RegisterStubContract` was exposed in `hle.cpp`.
A synthetic test in `tests/hle_stub_tests.cpp` verified:
- `SAFE` successfully returns `42` without crashing.
- `NEVER_SAFE` correctly triggers a `longjmp` and crashes with exit code 1.
- `UNKNOWN` correctly crashes with exit code 1.

## 4. SAFE Default Contract
A `SAFE` stub evaluates to a default policy. It must not have severe side effects and can be called repeatedly. Returning `0` is only appropriate if the PS5 API defines `0` as success or benign for that specific function.

## 5. UNKNOWN Semantics
Termination occurs at **first execution**. The diagnostic output contains:
- `MODULE: unknown` (or the specific PRX if linked via `RegisterNidDbStubs`)
- `NID: <raw string>`
- `SYMBOL: <friendly name>`
- `GUEST_RIP: <caller address>`
- `GUEST_RSP: <caller stack>`
- `STUB_CLASS: 6` (UNKNOWN)

## 6. NEVER_SAFE Semantics
`NEVER_SAFE` exists to explicitly declare that a function cannot have a meaningful default. It requires an implementation, interacts with external state (like a thread block or GPU queue), and must never execute without a real contract. Returning `0` would irreparably corrupt the guest state.

## 7. AGC Preparation
**Target**: `sceAgcSuspendPoint`
- **Classification**: `UNKNOWN` (6) - currently unpopulated in `g_stub_contracts`.
- **Link-time behavior**: Successfully resolves to an auto-stub thunk.
- **Runtime behavior**: Terminates the guest upon first call.
- **Current call site**: RIP `0x8000e72ad`
- **Arguments available**: Full `GuestArgs` structure (RDI, RSI, RDX, RCX, R8, R9, stack_args).
- **Reason it is unsafe**: Unknown semantics; guessing a return value or side effect for a GPU synchronization/submission primitive could cause race conditions, missing frames, or state corruption. Further characterization is required.

## Final Decision
**STUB CONTRACT VERIFIED**
The mechanism to safely auto-stub or strictly terminate the guest is functional and operates at the correct architectural boundary (execution time).

**Next Task**:
sceAgcSuspendPoint contract recovery.
