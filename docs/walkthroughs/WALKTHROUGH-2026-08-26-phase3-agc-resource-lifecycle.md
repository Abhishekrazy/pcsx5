# WALKTHROUGH-2026-08-26-phase3-agc-resource-lifecycle

## Execution
1. Hooked sceAgcDriverRegisterResource to trace inputs. Discovered argument 3 is the raw shader code pointer created by sceAgcCreateShader.
2. Restored sceAgcCreateShader to return SCE_OK (0), fixing a severe regression that caused a guest fprintf assertion failure on boot.
3. Implemented a unique atomic handle allocator for sceAgcDriverRegisterResource, properly writing the handle back to out_handle (dest + 0xC).
4. Re-ran PPSA21564. The game successfully allocated over 185 raytracing BVH shaders before crashing.
5. Investigated the crash: it is an emulator host memory-mapping bug (VEH Unhandled Exception in VCRUNTIME140.dll due to GuardedCopy skipping page-boundary demand-commits).
6. Halted implementation due to the realization that the resources are PlayStation Raytracing (sce::Psr) shaders which the Vulkan backend physically cannot support without major redesign.
7. Validated all new behaviors using hle_agc_tests.cpp.
