// GAL factory — creates the appropriate GpuDevice backend for the platform.
//
// Backend auto-probe priority (configurable via GalConfig::backend):
//   Auto   = Vulkan → GDI → Null
//   Vulkan = existing Vulkan backend
//   D3D12  = Direct3D 12 (future)
//   GDI    = GDI DIB software fallback
//   Null   = headless / testing

#include "gal.h"
#include "../common/log.h"

#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

// ---------------------------------------------------------------------------
// Forward declarations for each backend's Create function
// ---------------------------------------------------------------------------
// Each backend is declared in its own header; the factory includes only
// the headers for backends that are compiled on this platform.

#ifdef _WIN32
// Vulkan backend — dynamically loaded via volk-style loader, no SDK needed.
GpuDevice* CreateVulkanDevice(const GalConfig& config,
                               const GalWindowCallbacks& callbacks);
// GDI software fallback (Windows GDI DIB).
GpuDevice* CreateGdiDevice(const GalConfig& config,
                            const GalWindowCallbacks& callbacks);
#endif

// Null backend — always available.
GpuDevice* CreateNullDevice(const GalConfig& config,
                             const GalWindowCallbacks& callbacks);

// ---------------------------------------------------------------------------
// Backend probing helpers
// ---------------------------------------------------------------------------

// Returns true when the Vulkan loader DLL (vulkan-1.dll or libvulkan.so.1)
// can be loaded and a physical device enumerated.
static bool ProbeVulkanAvailable() {
#ifdef _WIN32
    HMODULE vulkan = ::LoadLibraryW(L"vulkan-1.dll");
    if (!vulkan) return false;
    ::FreeLibrary(vulkan);
    return true;
#else
    void* lib = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) return false;
    dlclose(lib);
    return true;
#endif
}

// ---------------------------------------------------------------------------
// GpuDevice::Create — factory entry point
// ---------------------------------------------------------------------------

GpuDevice* GpuDevice::Create(GalBackendType type) {
    GalConfig default_config{};
    GalWindowCallbacks default_callbacks{};
    return Create(type, default_config, default_callbacks);
}

GpuDevice* GpuDevice::Create(GalBackendType type,
                              const GalConfig& config,
                              const GalWindowCallbacks& callbacks) {
    const char* type_name = "Unknown";

    switch (type) {
        case GalBackendType::Auto: {
            // Probe priority: Vulkan → D3D12 → GDI → Null
            if (ProbeVulkanAvailable()) {
                LOG_INFO(GPU, "GAL: auto-probe: Vulkan available");
                GpuDevice* dev = CreateVulkanDevice(config, callbacks);
                if (dev) return dev;
                LOG_WARN(GPU, "GAL: Vulkan init failed; trying GDI fallback");
            }
#ifdef _WIN32
            GpuDevice* dev = CreateGdiDevice(config, callbacks);
            if (dev) return dev;
            LOG_WARN(GPU, "GAL: GDI init failed; using Null backend");
#endif
            return CreateNullDevice(config, callbacks);
        }

        case GalBackendType::Vulkan: {
            type_name = "Vulkan";
            if (!ProbeVulkanAvailable()) {
                LOG_ERROR(GPU, "GAL: Vulkan selected but vulkan-1.dll not found");
                return nullptr;
            }
            return CreateVulkanDevice(config, callbacks);
        }

#ifdef _WIN32
        case GalBackendType::GDI: {
            type_name = "GDI";
            return CreateGdiDevice(config, callbacks);
        }
#endif

        case GalBackendType::Null: {
            type_name = "Null";
            return CreateNullDevice(config, callbacks);
        }

        default:
            LOG_ERROR(GPU, "GAL: unknown backend type %d", static_cast<int>(type));
            return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Application-level convenience: create the best backend, log the result
// ---------------------------------------------------------------------------

GpuDevice* CreateBestGpuDevice(const GalConfig& config,
                                const GalWindowCallbacks& callbacks) {
    const GalBackendType requested = config.backend;
    GpuDevice* dev = GpuDevice::Create(requested, config, callbacks);
    if (dev) {
        GalCaps caps = dev->GetCaps();
        LOG_INFO(GPU, "GAL: created %s backend (adapter='%s', driver='%s')",
                 caps.backend_name.c_str(),
                 caps.adapter_name.c_str(),
                 caps.driver_version.c_str());
    } else {
        LOG_CRITICAL(GPU, "GAL: failed to create any GPU backend!");
    }
    return dev;
}
