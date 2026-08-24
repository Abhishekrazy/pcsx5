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

### 3. Current Boundary: Direct Memory Allocation
The runtime successfully completes `libc.prx` initialization using the authentic `SceProcParam` provided by `KernelGetProcParam`. `libc` transparently boots its own allocator and TLS structures. Execution transfers seamlessly to `eboot.bin`'s `_start`, initializes the C++ runtime, and enters the game engine, where it currently halts at the first unimplemented memory allocation stub: `sceKernelMapNamedDirectMemory`.
