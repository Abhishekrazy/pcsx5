# Task 15 Report: Guest Thread Stack Context & Raw Syscall Interception

## Root Causes Identified
1. **Thread Initialization Crash:** Worker threads were crashing instantly during pthread_create because InvokeGuestFunctionSafe in `src/cpu/cpu.cpp` was executing guest threads on the *host stack*, but with rdi (the thread argument) zeroed out due to a bug in `InvokeGuestOnStack`. This resulted in the thread attempting to read from `[rbx + 8]` where rbx was 0.
2. **Raw Syscall Crashes:** Even when guest threads survived initialization, fatal aborts (like `std::terminate`) in the guest game would invoke raw FreeBSD syscalls (`int 0x41`) directly. The VEH treated these as unhandled `0xC0000005` (Access Violation) exceptions.
3. **STATUS_BAD_STACK on Worker Threads:** Moving the guest threads to execute on the *guest stack* triggered fatal `0xC0000028` (STATUS_BAD_STACK) errors when an exception (like `int 0x41`) occurred. This happened because TEB->StackBase and StackLimit spoofing was only applied to the main thread in `src/kernel/kernel.cpp`, but omitted in the worker thread entrypoint in `src/cpu/cpu.cpp`.

## Fixes Implemented
1. **Fixed `InvokeGuestOnStack`:** Corrected the assembly in `src/hle/dispatcher.asm` to accept and set rdi_arg (the third argument) into rdi before switching to the guest stack.
2. **Fixed Worker Thread Stacks:** Updated `src/cpu/cpu.cpp` to correctly calculate guest_sp and use `InvokeGuestOnStack` instead of `InvokeGuestFunction`, ensuring worker threads execute natively on their allocated guest stacks.
3. **Worker Thread TEB Spoofing:** Added TEB->StackBase and StackLimit spoofing inside `InvokeGuestFunctionSafe` to ensure Windows SEH dispatch can safely evaluate exceptions occurring on the guest stack.
4. **Raw Syscall (`int 0x41`) Interception:** Added explicit detection for `0xCD 0x41` (`int 0x41`) in the `VectoredExceptionHandler`. It now routes `sys_exit` (1) to a clean emulator shutdown (`TerminateProcess` with log flushes) and `sys_thr_exit` (431) to `TerminateThread`.

## Results
- The thread initialization crash is fully resolved. Worker threads (BackgroundTaskWorker) spawn successfully and process work.
- The raw sys_exit execution is correctly caught and mapped to a clean shutdown.
- **Verification:** As requested, successfully verified that PPSA02929 achieves a successful first-frame presentation (drawing via AGC and flipping the swapchain) without memory corruption or stack unwinding errors.
