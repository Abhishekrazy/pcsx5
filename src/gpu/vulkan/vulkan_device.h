// VulkanDevice — wraps the existing Vulkan backend behind the GAL
// GpuDevice interface.  Most methods delegate to the existing
// vk_context / vk_present / vk_draw code; future refactoring can
// move logic here incrementally.

#pragma once
#include "../gal.h"

// Create a Vulkan-backed GpuDevice.  Returns nullptr if the Vulkan
// DLL is not found or device initialisation fails.
GpuDevice* CreateVulkanDevice(const GalConfig& config,
                               const GalWindowCallbacks& callbacks);
