# PS5 Runtime Lifecycle

## Kernel vs Guest Ownership Boundaries

### 1. PRX Initialization (Kernel Owned)
The emulator Kernel guarantees that for all dynamically linked PRX libraries:
- DT_INIT and DT_INIT_ARRAY are executed exactly once.
- Execution occurs in dependency-first topological order.
- Execution runs on the guest stack, invoked by the host Kernel::Execute.

### 2. Eboot Initialization (Guest Owned)
The emulator transfers control to the eboot.bin entry point (_start), which is responsible for:
- Initializing the C/C++ environment (_init_env).
- Calling its own DT_INIT and global constructors.
- Invoking main(argc, argv, envp) via XKRegsFpEpk or native runtime equivalent.

### 3. Engine Initialization & Assets
The engine natively spawns worker threads (e.g. for JSON parsing) via scePthreadCreate. Guest strings are tokenized natively using guest logic (uncovering engine-specific defects like the count=6 truncation bug). These threads use guest stack and TLS memory mapped by the host VirtualAlloc.

### 4. Thread Teardown (Corrected)
When a worker thread completes its task, it calls scePthreadExit. This invokes Kernel::ExitThread(exit_code), which ensures:
1. **TEB Restoration**: The host TEB StackBase (GS:0x08) and StackLimit (GS:0x10) are restored to their original values. This prevents VCRUNTIME140D.dll from encountering an Access Violation during DLL_THREAD_DETACH.
2. **Resource Cleanup**: CpuCore::HandleThreadExit is called, which properly frees the guest stack, guest TLS, and clears the thread's is_running flag.
3. **Graceful OS Exit**: Only after emulator teardown is complete does the thread invoke ::ExitThread, ensuring the host process state remains uncorrupted.
