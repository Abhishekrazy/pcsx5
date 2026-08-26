# Walkthrough: Recovering sceAgcDriverRegisterOwner & Worker Teardown (Gate A & B)

## Overview
This walkthrough details the steps taken to recover the calling contract for sceAgcDriverRegisterOwner, resolve a hidden assertion failure in sceAgcCreateShader, and fix a critical ntdll crash during worker teardown.

## Step 1: Interception Strategy (Gate A)
We registered a temporary HLE hook for sceAgcDriverRegisterOwner that dumped RDI, RSI, and the caller's instructions.
- RDI was a pointer to .bss.
- RSI contained "sce::Psr".
- The caller checked for eax == 0.
This proved the function allocates an owner handle, writes it to [RDI], and returns 0.

## Step 2: Resolving sceAgcCreateShader (Gate A)
After implementing the owner registration, the guest crashed with a failed assertion: ssertion 'error == 0' failed.
We corrected sceAgcCreateShader to return SCE_OK (0) instead of a pointer, resolving the crash.

## Step 3: Reproducing the Worker Teardown Crash (Gate B)
PPSA02929 exhibited an access violation in VCRUNTIME140D.dll when background threads were forcibly shut down.
We identified that PthreadExitImpl (in libkernel.cpp) called the Win32 ::ExitThread, skipping Kernel::ExitThread.

## Step 4: Fixing Teardown Boundaries (Gate B)
We fixed PthreadExitImpl to call Kernel::ExitThread(exit_value). This ensured that:
1. Host TEB (GS:0x08, GS:0x10) stack bounds are restored, so Windows DLL_THREAD_DETACH works without crashing.
2. CpuCore::HandleThreadExit is called, correctly releasing guest stack and TLS memory for detached threads.
3. Thread records are accurately tracked, preventing CpuCore::Shutdown from incorrectly terminating threads.

We also updated ExitGuestProcess in hle.cpp to cleanly exit worker threads instead of relying on ::TerminateProcess. All tests now pass cleanly at 100%.
