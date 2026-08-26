# Task: PPSA21564 sceAgcDriverRegisterOwner CONTRACT RECOVERY & WORKER TEARDOWN (Gate A & B)

## Status
DONE

## Objective
Gate A: Recover the precise calling contract for sceAgcDriverRegisterOwner without guessing or faking success, and implement it.
Gate B: Determine the FIRST incorrect state during worker teardown causing the ntdll access violation and correct ownership/destruction paths.

## Evidence & Analysis (Gate A)
We instrumented sceAgcDriverRegisterOwner to dump the host registers and caller code.
The trace revealed:
- **RDI**: Points to an output buffer in guest .bss
- **RSI**: Points to a null-terminated string ("sce::Psr")
- **Caller Code**: expects 0 for success.
We established the contract: int sceAgcDriverRegisterOwner(int32_t* out_handle, const char* name);
We also discovered that sceAgcCreateShader was incorrectly returning a pointer instead of SCE_OK (0). We fixed this, which allowed the guest to advance.

## Evidence & Analysis (Gate B)
Investigation revealed that scePthreadExit in src/hle/libkernel.cpp was incorrectly bound to the Win32 ::ExitThread function because it lacked the Kernel:: namespace qualifier.
This resulted in thread teardown completely bypassing the emulator's Kernel::ExitThread logic.
As a result:
1. The thread's host TEB stack bounds were never restored to their original OS values, causing VCRUNTIME140D.dll to crash with an Access Violation during DLL_THREAD_DETACH.
2. CpuCore::HandleThreadExit was never called, leaking the guest stack, guest TLS, and WakeEvent handle for detached threads.
3. Thread records were left as is_running = true, causing CpuCore::Shutdown() to issue ::TerminateThread on already-dead handles.

Furthermore, ExitGuestProcess in src/hle/hle.cpp fell back to ::TerminateProcess when invoked off the main thread (e.g. from an unknown stub crash). This caused immediate abrupt teardown which exacerbates background thread instability.

## Implementation (Gate B)
- Patched src/hle/libkernel.cpp to explicitly invoke Kernel::ExitThread(exit_value).
- Patched src/hle/hle.cpp so that ExitGuestProcess requests a cooperative stop and gracefully exits the active worker thread via Kernel::ExitThread.
- This ensures all threads correctly clean up guest resources and restore OS TEB limits before native teardown. CTest passes at 100%.

## Next Architectural Task
**TASK 23**: PPSA21564 sceAgcDriverRegisterResource CONTRACT RECOVERY.
The guest now halts at this new NID DB stub. We must capture its arguments, trace its caller, and define the contract.
