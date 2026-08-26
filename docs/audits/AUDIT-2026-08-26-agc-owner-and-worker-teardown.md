# Audit: sceAgcDriverRegisterOwner Contract & Worker Teardown (Gate A & B)

## Boundary Inspected
- sceAgcDriverRegisterOwner (libSceAgcDriver / libSceAgc)
- sceAgcCreateShader (libSceAgc)
- Worker Thread Teardown (Kernel::ExitThread vs ::ExitThread)

## Observed Guest State (PPSA21564) (Gate A)
- Trace captured during early background worker initialization.
- **RDI**: .bss output pointer for the returned handle.
- **RSI**: pointer to "sce::Psr"
- **Return Check**: test eax, eax indicating 0 is SCE_OK.
Following this, the guest calls sceAgcCreateShader, which also expects 0 as the return code, proving previous implementations returning a header pointer were incorrect.

## Observed Teardown State (Gate B)
- Background worker threads (e.g. Astro's Playroom audio/video workers) triggered an ntdll / VCRUNTIME140D.dll access violation upon emulator shutdown or unknown-stub crash.
- Investigation traced the root cause to PthreadExitImpl in src/hle/libkernel.cpp. Due to namespace resolution, it bound to Win32 ::ExitThread, entirely bypassing the Kernel::ExitThread sequence.
- This skipped the host TEB bounds restoration (GS:0x08, GS:0x10), leaving them pointing to the guest stack. Native DLL_THREAD_DETACH handlers in the CRT then crashed while attempting to read the stack state.
- It also skipped CpuCore::HandleThreadExit, leaking guest memory for detached threads and leaving zombie thread handles that CpuCore::Shutdown() improperly attempted to TerminateThread.

## Actions Taken
1. Implemented sceAgcDriverRegisterOwner using an atomic u32 handle generator.
2. Modified sceAgcCreateShader to return 0 instead of the header pointer.
3. Modified libkernel.cpp to call Kernel::ExitThread.
4. Modified ExitGuestProcess in hle.cpp to use HLE::RequestStop() and Kernel::ExitThread() for worker threads, avoiding asynchronous TerminateProcess destruction.
