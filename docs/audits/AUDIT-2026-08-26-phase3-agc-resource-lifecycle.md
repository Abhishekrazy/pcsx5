# AUDIT-2026-08-26-phase3-agc-resource-lifecycle

## Boundary Ledger

| Boundary | Title | RIP | Function/NID | Evidence | Classification | Fix | Regression | Result |
|---|---|---|---|---|---|---|---|---|
| AGC Shader Lifecycle | PPSA21564 | N/A | sceAgcCreateShader | Game asserts error == 0 when header is returned. | **VERIFIED** | Return 0 (SCE_OK). | PPSA02929 OK | Game continues allocation |
| AGC Resource Registration | PPSA21564 | N/A | sceAgcDriverRegisterResource | ABI confirmed via pointer tracing: int sceAgcDriverRegisterResource(uint32_t* out_handle, uint32_t owner, void* code, uint32_t size) | **VERIFIED** | Atomic handle generator | PPSA02929 OK | Shaders successfully registered |
| AGC GPU Raytracing | PPSA21564 | N/A | IMAGE_BVH_INTERSECT_RAY | Game initializes PlayStation Raytracing (sce::Psr). Vulkan lacks backend integration. | **OBSERVED** | N/A | N/A | **STOPPED AT ARCHITECTURAL BOUNDARY** |
