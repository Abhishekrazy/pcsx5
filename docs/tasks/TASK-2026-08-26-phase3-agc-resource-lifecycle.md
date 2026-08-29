# TASK-2026-08-26-phase3-agc-resource-lifecycle

## Objective
Recover and implement the REAL AGC resource-registration lifecycle for PPSA21564, tracing resource allocation and resolving crashes.

## Findings
- **sceAgcDriverRegisterResource**: Discovered that the object registered is the shader code pointer itself. Found that it requires a 32-bit handle generated and written to dest+12 (the SceAgcShader::handle field) and must return 0 (SCE_OK).
- **sceAgcCreateShader Regression**: Discovered that a previous modification causing it to return header instead of 0 caused PPSA21564 to hit a guest-side assertion (psr_gpu_interface.cpp(line 2986): assertion 'error == 0' failed). Reverted this to return 0 (SCE_OK).
- **Resource Exhaustion Bug**: The reported "resource exhaustion" in the host is actually a guest memcpy access violation inside libkernel::memcpy (GuardedCopy). The guest is issuing a massive memory copy that crosses a page boundary into uncommitted memory. The emulator's memory mapping architecture triggers a VEH crash in VCRUNTIME140.dll rather than a graceful page fault. This is a multi-threaded memory emulation bug.
- **Raytracing Architecture Boundary**: The game is allocating shaders via psr_gpu_interface.cpp (PlayStation Raytracing). The shaders being registered are BVH traversal and intersection shaders. The emulator's Vulkan backend and GCN decoder currently do not support RDNA2 raytracing instructions.

## Status
Stopped at the architectural boundary. BVH raytracing shaders cannot be correctly executed by the current emulator backend. All recovered AGC mock-ups have been verified with CTest. PPSA02929 remains regression-free.
