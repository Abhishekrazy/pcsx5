// FSR upscaling — AMD FidelityFX Super Resolution 2.x integration.
//
// Dynamically loads ffx_fsr2_api_vk.dll at runtime.  When the DLL is
// not found, all operations are no-ops and the emulator renders at
// native resolution without upscaling.

#include "fsr_upscale.h"
#include "../common/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// ===========================================================================
// FSR 2 resolution helpers
// ===========================================================================

void FsrUpscale::GetRenderResolution(FsrQuality quality,
                                      uint32_t display_w, uint32_t display_h,
                                      uint32_t& out_render_w,
                                      uint32_t& out_render_h) {
    float scale = 1.0f;
    switch (quality) {
        case FsrQuality::Quality:          scale = 1.5f;  break;
        case FsrQuality::Balanced:         scale = 1.7f;  break;
        case FsrQuality::Performance:      scale = 2.0f;  break;
        case FsrQuality::UltraPerformance: scale = 3.0f;  break;
        default:                           scale = 1.0f;  break;
    }
    out_render_w = static_cast<uint32_t>(
        static_cast<float>(display_w) / scale + 0.5f);
    out_render_h = static_cast<uint32_t>(
        static_cast<float>(display_h) / scale + 0.5f);
}

// ===========================================================================
// FSR 2 DLL detection
// ===========================================================================

bool FsrUpscale::IsAvailable() {
    HMODULE dll = ::LoadLibraryW(L"ffx_fsr2_api_vk.dll");
    if (dll) {
        ::FreeLibrary(dll);
        return true;
    }
    // Also check the AMD folder (common install location).
    dll = ::LoadLibraryW(L"C:\\Program Files\\AMD\\FidelityFX\\FSR2\\ffx_fsr2_api_vk.dll");
    if (dll) {
        ::FreeLibrary(dll);
        return true;
    }
    return false;
}

// ===========================================================================
// Lifecycle
// ===========================================================================

FsrUpscale::~FsrUpscale() {
    Shutdown();
}

bool FsrUpscale::Initialize(VkDevice device, VkPhysicalDevice phys_device,
                             VkQueue queue, uint32_t queue_family,
                             const FsrConfig& config) {
    (void)device;
    (void)phys_device;
    (void)queue;
    (void)queue_family;

    if (m_initialized) Shutdown();

    if (config.quality == FsrQuality::Off) {
        LOG_INFO(GPU, "FSR: disabled (config)");
        return false;
    }

    if (!IsAvailable()) {
        LOG_WARN(GPU, "FSR: ffx_fsr2_api_vk.dll not found — install AMD FidelityFX SDK");
        return false;
    }

    m_config = config;

    // FIXME: full FSR 2 integration requires:
    //   1. Load ffx_fsr2_api_vk.dll and resolve function pointers
    //   2. Create Fsr2Context with Fsr2CreateContext()
    //   3. Create internal resources for the upscaled output
    //   4. Each frame: dispatch FSR 2 compute shaders then RCAS sharpen
    //
    // The DLL provides C-style exports:
    //   Fsr2CreateContext(VkDevice, VkPhysicalDevice, ...)
    //   Fsr2Dispatch(Upscale, ...)
    //   Fsr2DestroyContext()
    //
    // Until the AMD FidelityFX SDK headers are integrated, this is a stub.

    LOG_WARN(GPU, "FSR: DLL found but integration incomplete — FSR SDK headers needed");
    m_initialized = false;
    return false;
}

void FsrUpscale::Shutdown() {
    if (m_dll) {
        ::FreeLibrary(static_cast<HMODULE>(m_dll));
        m_dll = nullptr;
    }
    m_initialized = false;
}

bool FsrUpscale::CreateResolutionResources(uint32_t /*render_w*/,
                                            uint32_t /*render_h*/,
                                            uint32_t /*display_w*/,
                                            uint32_t /*display_h*/) {
    if (!m_initialized) return false;
    // FIXME: dispatch to Fsr2CreateContext() with new resolution
    return false;
}

void FsrUpscale::Upscale(VkCommandBuffer /*cmd*/, const FsrInput& /*input*/) {
    // FIXME: dispatch Fsr2Dispatch() then RCAS sharpen
}
