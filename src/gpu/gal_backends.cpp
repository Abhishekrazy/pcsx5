// GAL backend creation stubs.
//
// Each backend is created by a standalone function declared in gal.cpp.
// This file provides the implementations; backends that have not yet
// been ported to the GAL interface return nullptr (causing the factory
// to fall through to the next backend or the Null device).

#include "gal.h"
#include "vulkan/vulkan_device.h"
#include "../common/log.h"

// ===========================================================================
// Vulkan backend — wraps existing vk_context/vk_present/vk_draw
// behind the GpuDevice interface via VulkanDevice adapter class.
// ===========================================================================

GpuDevice* CreateVulkanDevice(const GalConfig& config,
                               const GalWindowCallbacks& callbacks);

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
