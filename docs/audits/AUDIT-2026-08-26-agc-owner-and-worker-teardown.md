# Audit: sceAgcDriverRegisterOwner Contract & Worker Teardown (Gate A & B)

## Boundary Inspected
- sceAgcDriverRegisterOwner (libSceAgcDriver / libSceAgc)
- sceAgcCreateShader (libSceAgc)
- Worker Thread Teardown (Kernel::ExitThread vs ::ExitThread)
- Process vs Thread Exit Semantics (ExitGuestProcess)

## Observed Guest State (PPSA21564) (Gate A)
- Trace captured during early background worker initialization.
- **RDI**: .bss output pointer for the returned handle.
- **RSI**: pointer to "sce::Psr"
- **Return Check**: test eax, eax indicating 0 is SCE_OK.
Following this, the guest calls sceAgcCreateShader, which also expects 0 as the return code, proving previous implementations returning a header pointer were incorrect.

## Process vs Thread Teardown (Gate B)
- Background worker threads (e.g. Astro's Playroom audio/video workers) triggered an ntdll / VCRUNTIME140D.dll access violation upon emulator shutdown or unknown-stub crash.
- Investigation traced the root cause to PthreadExitImpl in src/hle/libkernel.cpp. Due to namespace resolution, it bound to Win32 ::ExitThread, entirely bypassing the Kernel::ExitThread sequence.
- This skipped the host TEB bounds restoration (GS:0x08, GS:0x10), leaving them pointing to the guest stack. Native DLL_THREAD_DETACH handlers in the CRT then crashed while attempting to read the stack state.
- **TLS Leak Fix**: Investigation of HandleThreadExit revealed that VirtualFree was passing thread.tls_base directly, but tls_base points 0x10000 bytes into the VirtualAlloc block. We patched FreeThreadGuestMemory to subtract 0x10000, fixing a permanent emulator-wide TLS leak.
- **Process Semantics**: ExitGuestProcess was audited. It handles *process-wide fatal exits* (e.g. UNKNOWN stub, fatal exception, sys_exit, std::terminate). Using ::TerminateProcess for this is strictly correct, as it purposefully halts the process and bypasses thread CRT teardown, correctly mirroring a fatal crash without triggering the VCRUNTIME access violation.

## Actions Taken
1. Implemented sceAgcDriverRegisterOwner using an atomic u32 handle generator.
2. Modified sceAgcCreateShader to return 0 instead of the header pointer.
3. Modified libkernel.cpp to call Kernel::ExitThread.
4. Fixed a VirtualFree memory leak in CpuCore::FreeThreadGuestMemory.
5. Created a repeated thread stress test in worker_libc_abi_tests.cpp to prove zero leakage for both detached and joinable threads.
