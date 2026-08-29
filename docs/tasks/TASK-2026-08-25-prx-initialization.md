# TASK 13: Kernel-Driven PRX Initialization Queue

## Objective
Recover and implement the real PRX initialization lifecycle, migrating ownership of PRX DT_INIT and DT_INIT_ARRAY execution from the guest (which was proven not to handle it) to the emulator's Kernel loader, prior to transferring control to eboot _start.

## Implementation
1. **ELF Metadata Extension**: Extended Loader::LoadedModule to extract DT_INIT_ARRAY and DT_INIT_ARRAYSZ alongside DT_INIT.
2. **PRX Initialization Queue**: Modified Kernel::LinkLoadedPrxModules to populate a dependency-ordered initialization queue for all loaded PRXs.
3. **Guest Execution Environment**: Added InvokeGuestOnStack to dispatcher.asm to execute guest PRX initialization functions on the dedicated, aligned guest stack rather than the host stack.
4. **Execution Lifecycle**: Injected the execution of the PRX initialization queue into Kernel::Execute immediately before StartGuestCaptured.
5. **Cleanup**: Removed the architecturally obsolete loop inside libkernel::XKRegsFpEpk (which was never legitimately reached).

## Re-Characterization Results
- **PPSA21564**: Execution successfully invoked libSceNpCppWebApi.prx DT_INIT, then proceeded to libc.prx DT_INIT. During libc.prx DT_INIT, execution aborted via sceKernelDebugRaiseException(0xa0020013).
- **PPSA02929**: Execution proceeded directly to libc.prx DT_INIT, which immediately aborted via sceKernelDebugRaiseException(0xa0020013).
- **Outcome**: The std::bad_alloc crash during eboot DT_INIT is resolved (as execution never incorrectly reaches it). The emulator has now hit a genuine, earlier architectural boundary: the guest environment provided to libc DT_INIT lacks a valid PS5 kernel state or structure expected by libc.

## Claims vs Reality
| Claim | Classification | Proof |
|-------|----------------|-------|
| PRX DT_INIT runs before _start | VERIFIED | docs/evidence/2026-08/task-13-prx-init/boot_log12_debug.txt traces MODULE_INIT_BEGIN before GUEST_ENTRY_BEGIN. |
| PRX constructors run on guest stack | IMPLEMENTED | InvokeGuestOnStack switches rsp to sp - 1024. |
| libc DT_INIT requires additional kernel state | OBSERVED | libc aborts with 0xa0020013 inside its DT_INIT_ARRAY. |
| eboot _start executes PRX initializers | HACK (Obsolete) | Removed. Verified that eboot _start does not do this natively. |

## Next Task
**TASK 14: Investigate 0xa0020013 during libc DT_INIT**
- **Objective:** Determine the meaning of 0xa0020013 (likely a Thread/Kernel environment error) and establish the expected kernel structures or syscalls that libc DT_INIT relies on before it aborts.
