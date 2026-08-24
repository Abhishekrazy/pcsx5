# PS5 Boot Pipeline

## Current Stage Classification
**Stage:** EARLY MODULE INITIALIZATION

## Pipeline
1. **Host Setup:** Initialize emulator components (Memory, Logger, Config, HLE, GPU).
2. **ELF Loading:** Decrypt/parse main executable (`eboot.bin`) into guest memory.
3. **Dependency Loading:** Load recursive PRX dependencies (e.g. `libc.prx`, `libkernel.prx`) and apply segments.
4. **Linker:** Resolve imported symbols (NID matching). Unresolved symbols map to HLE `ResolveAny` (crash stubs).
5. **Execution Start:** Set up main thread guest stack (w/ auxiliary vector), bind TLS, and start execution loop `Kernel::Execute`.
6. **PRX Initialization:** The Kernel iterates the dependency graph in topological order, executing `DT_INIT` and `DT_INIT_ARRAY` for each PRX on the guest stack. *(CURRENT BOTTLENECK - libc.prx crashes at 0x82000aaff after successfully bypassing the 0xa0020013 exception by satisfying KernelGetProcParam)*
7. **Main Module Initialization:** Jump to main module's `e_entry` (`_start`), which calls `_init_env`, then runs `eboot` `DT_INIT`.
8. **Guest Execution:** Enter `main()` or guest specific runtime.
