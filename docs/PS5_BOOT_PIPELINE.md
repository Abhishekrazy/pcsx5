# PS5 Boot Pipeline

## Current Stage Classification
**Stage:** GUEST RUNTIME / COMMAND BUFFER

## Pipeline
1. **Host Setup:** Initialize emulator components (Memory, Logger, Config, HLE, GPU).
2. **ELF Loading:** Decrypt/parse main executable (eboot.bin) into guest memory.
3. **Dependency Loading:** Load recursive PRX dependencies (e.g. libc.prx, libkernel.prx) and apply segments.
4. **Linker:** Resolve imported symbols (NID matching). Unresolved symbols map to HLE ResolveAny (crash stubs).
5. **Execution Start:** Set up main thread guest stack (w/ auxiliary vector), bind TLS, and start execution loop Kernel::Execute.
6. **PRX Initialization:** The Kernel iterates the dependency graph in topological order, executing DT_INIT and DT_INIT_ARRAY for each PRX on the guest stack.
7. **Main Module Initialization:** Jump to main module's e_entry (_start), which calls _init_env, then runs eboot DT_INIT.
8. **Guest Execution:** Enter main().
9. **Guest Runtime:** Thread pools spawn, graphics engine initializes, shaders load, and JSON configuration/assets are parsed.
10. **AGC Owner Registration:** Driver owners are registered via sceAgcDriverRegisterOwner, and sceAgcCreateShader succeeds.
11. **Worker Thread Teardown:** Worker threads exit cleanly via Kernel::ExitThread, correctly restoring host TEB bounds and freeing guest resources.
12. **AGC Resource Registration:** *(CURRENT BOTTLENECK - Guest halted at sceAgcDriverRegisterResource stub awaiting implementation).*
