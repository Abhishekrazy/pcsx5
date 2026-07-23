// GAL backend creation stubs.
//
// Each backend is created by a standalone function declared in gal.cpp.
// This file provides the implementations; backends that have not yet
// been ported to the GAL interface return nullptr (causing the factory
// to fall through to the next backend or the Null device).

#include "gal.h"
#include "../common/log.h"

// ===========================================================================
// Vulkan backend — NOT YET PORTED TO GAL INTERFACE
// ===========================================================================
// The existing Vulkan code (vk_context, vk_present, vk_draw) still uses
// the legacy GPU::* API (gpu.h).  Until it is refactored into a
// VulkanDevice : GpuDevice subclass, this factory returns nullptr.
// The legacy path is driven through GPU::Initialize() / RenderFrame()
// in gpu.h as before.

GpuDevice* CreateVulkanDevice(const GalConfig& /*config*/,
                               const GalWindowCallbacks& /*callbacks*/) {
    LOG_WARN(GPU, "GAL: Vulkan backend not yet ported to GpuDevice interface; "
                  "use the legacy GPU::* API for now.");
    return nullptr;
}

// ===========================================================================
// GDI backend — NOT YET PORTED TO GAL INTERFACE
// ===========================================================================
// The existing GDI DIB boot-screen path is called through GPU::SetBootStatus()
// and not yet wrapped in a GdiDevice class.

GpuDevice* CreateGdiDevice(const GalConfig& /*config*/,
                            const GalWindowCallbacks& /*callbacks*/) {
    LOG_WARN(GPU, "GAL: GDI backend not yet ported to GpuDevice interface; "
                  "use the legacy GPU::* API for now.");
    return nullptr;
}

// ===========================================================================
// Null backend — immediately available, trivially implemented
// ===========================================================================
// The NullGpuDevice class is defined inline in gal.h.

GpuDevice* CreateNullDevice(const GalConfig& /*config*/,
                             const GalWindowCallbacks& /*callbacks*/) {
    return new NullGpuDevice();
}
