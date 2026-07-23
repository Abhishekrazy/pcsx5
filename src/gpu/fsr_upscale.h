// FSR 2.x / 3.x upscaling integration.
//
// AMD FidelityFX Super Resolution provides:
//   - FSR 2.x: Temporal upscaling with quality presets
//   - FSR 3.x: Frame generation (AFMF) + FSR 2 quality
//
// The FSR 2 SDK is MIT-licensed and available from:
//   https://github.com/GPUOpen-Effects/FidelityFX-FSR2
//
// Integration approach:
//   - Dynamically load `ffx_fsr2_api_vk.dll` at runtime
//   - Create FSR 2 context from the emulator's VkDevice + VkPhysicalDevice
//   - Feed guest render target + motion vectors → FSR 2 → RCAS sharpen → present
//
// Config (GraphicsConfig):
//   fsr = off | quality | balanced | performance | ultra_performance
//   fsr_sharpness = 0.0..1.0

#pragma once
#include "../common/types.h"
#include <cstdint>
#include <string>

// Forward declarations for Vulkan types (include vulkan.h before this
// if you need VkImage access).
#ifndef VK_DEFINE_HANDLE
// Minimal forward decl for header-only use.
#define VK_DEFINE_HANDLE(name) struct name##_T; typedef name##_T* name;
VK_DEFINE_HANDLE(VkImage)
VK_DEFINE_HANDLE(VkDevice)
VK_DEFINE_HANDLE(VkPhysicalDevice)
VK_DEFINE_HANDLE(VkCommandBuffer)
VK_DEFINE_HANDLE(VkQueue)
VK_DEFINE_HANDLE(VkSampler)
VK_DEFINE_HANDLE(VkImageView)
#undef VK_DEFINE_HANDLE
#endif

// FSR quality presets (determines render resolution scale).
enum class FsrQuality : int {
    Off              = 0,
    Quality          = 1,   // 1.5x scale (e.g. 853→1280, 1280→1920)
    Balanced         = 2,   // 1.7x scale
    Performance      = 3,   // 2.0x scale (e.g. 960→1920, 1440→2880)
    UltraPerformance = 4,   // 3.0x scale
};

// FSR configuration (subset of GraphicsConfig).
struct FsrConfig {
    FsrQuality quality          = FsrQuality::Off;
    float      sharpness        = 0.5f;   // RCAS sharpness 0.0..1.0
    bool       enable_frame_gen = false;  // FSR 3 AFMF (future)
};

// Per-frame FSR input data.
struct FsrInput {
    // Color buffer (current frame, low res).
    VkImage  color          = nullptr;
    VkImage  depth          = nullptr;    // depth buffer (optional)
    VkImage  motion_vectors = nullptr;    // 2D velocity buffer (required for FSR 2)
    VkImage  exposure       = nullptr;    // exposure (auto-exposure, optional)
    VkImage  reactive_mask  = nullptr;    // reactive mask (optional)

    uint32_t render_width   = 0;          // low-res render resolution
    uint32_t render_height  = 0;
    uint32_t output_width   = 0;          // display resolution
    uint32_t output_height  = 0;

    // Camera jitter for temporal stability (pixel offsets).
    float    jitter_x       = 0.0f;
    float    jitter_y       = 0.0f;

    // Frame count (monotonic, for temporal accumulation).
    uint32_t frame_count    = 0;

    // If true, reset temporal accumulation (seek, cut, camera cut).
    bool     reset          = false;
};

// FSR upscaling context — wraps the FSR 2 API.
class FsrUpscale {
public:
    FsrUpscale() = default;
    ~FsrUpscale();

    // Prevent copy.
    FsrUpscale(const FsrUpscale&) = delete;
    FsrUpscale& operator=(const FsrUpscale&) = delete;

    // ---- lifecycle -------------------------------------------------------
    // Initialize the FSR 2 context.  Requires a Vulkan 1.2+ device with
    // VK_KHR_get_physical_device_properties2 and 16-bit storage features.
    // `device` and `phys_device` are the emulator's VkDevice/VkPhysicalDevice.
    // Returns false if the FSR 2 DLL is not found or the device is unsuitable.
    bool Initialize(VkDevice device, VkPhysicalDevice phys_device,
                    VkQueue queue, uint32_t queue_family,
                    const FsrConfig& config);

    // Shutdown and release all FSR resources.
    void Shutdown();

    // Re-create internal resources for a new resolution (called on resize).
    bool CreateResolutionResources(uint32_t render_w, uint32_t render_h,
                                    uint32_t display_w, uint32_t display_h);

    // ---- upscale ---------------------------------------------------------
    // Execute the full FSR 2 upscale + RCAS sharpen on the given command
    // buffer.  Call between Begin/EndRenderPass as necessary.
    void Upscale(VkCommandBuffer cmd, const FsrInput& input);

    // ---- helpers ---------------------------------------------------------
    // Get the render resolution for a given display resolution and quality.
    static void GetRenderResolution(FsrQuality quality,
                                     uint32_t display_w, uint32_t display_h,
                                     uint32_t& out_render_w,
                                     uint32_t& out_render_h);

    // True when the FSR 2 DLL is available on the system.
    static bool IsAvailable();

    const FsrConfig& GetConfig() const { return m_config; }
    bool IsInitialized() const { return m_initialized; }

private:
    bool m_initialized = false;
    FsrConfig m_config{};

    // Opaque handle to the FSR 2 API context.
    void* m_fsr2_context = nullptr;

    // Output (upscaled + sharpened) image.
    VkImage m_output_image = nullptr;
    VkImageView m_output_view = nullptr;

    // Loaded DLL handle.
    void* m_dll = nullptr;
};

// FSR is an optional feature; call IsInitialized() before Upscale().
