# AUDIT: P0 Boot Truth Recovery & False-Success Eradication

## Goal
Identify the true, unmasked first point of failure for PPSA02929 by eliminating false-success execution paths (Fast Sentinel Recovery and 0-returning HLE auto-stubs for unknown symbols).

## Methodology
1. **Disable Fast Sentinel Recovery**: Removed the host-level VEH hack (`0xFFFFFFFF8xxxxxxx` suppression) that previously caught unhandled page faults and zeroed `RCX` to blindly continue execution.
2. **Strict HLE Auto-stubbing**: Implemented a contract-based `GetStubContract` check for `HLE::ResolveAny` and `RegisterNidDbStubs`. Any symbol classified as `UNKNOWN` or `NEVER_SAFE` now forces an immediate termination (`ExitGuestProcess(1)`) with a fatal log (`UNSUPPORTED GUEST OPERATION`), rather than returning 0 and corrupting guest state.
3. **Execution with PPSA02929**: Ran the updated emulator binary against `eboot.bin` to observe the genuine crash point.

## Findings

When run from the standard path (with `assets/nid_db.txt` correctly loaded):
- **First True Failure**: The emulator reached the GPU subsystem and successfully rendered/presented a frame. Immediately afterward, it requested an unknown stub: `sceAgcSuspendPoint`.
- **Log Extract**:
```
[HLE][Error] ==================================================
[HLE][Error] UNSUPPORTED GUEST OPERATION (NID DB STUB)
[HLE][Error] MODULE: libSceAgc
[HLE][Error] NID: sceAgcSuspendPoint
[HLE][Error] SYMBOL: sceAgcSuspendPoint
[HLE][Error] GUEST_RIP: 0x8000e72ad
[HLE][Error] GUEST_RSP: 0x318efef518
[HLE][Error] STUB_CLASS: 6
[HLE][Error] ==================================================
```

### Additional Finding: Missing NID Database Fallback
During testing, running `pcsx5_cli.exe` from a directory where `assets/nid_db.txt` was inaccessible triggered a failure much earlier at:
- **Module**: `unknown`
- **NID**: `F8bUHwAG284#A#B`
This corresponds to `scePthreadMutexattrInit`. Because the NID DB was missing, `F8bUHwAG284` was not resolved to its friendly name, bypassing the existing real HLE implementation in `libkernel_sync.cpp` and triggering the strict `ExitGuestProcess(1)`. Running with the DB loaded properly resolved it and allowed execution to reach `sceAgcSuspendPoint`.

## Conclusion
The architectural changes successfully stripped away the speculative execution that previously drove the emulator into a corrupt state (STATUS_BAD_STACK). We now have deterministic evidence that `PPSA02929` fails at `sceAgcSuspendPoint`. The boot pipeline has been updated to reflect the removal of the Fast Sentinel Recovery hack.
