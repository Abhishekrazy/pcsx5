// VulkanDevice — GAL GpuDevice implementation wrapping the existing
// Vulkan backend code.  Currently delegates to the global GPU::* API;
// incrementally move functionality here as the GAL interface matures.

#include "vulkan_device.h"
#include "../vk_present.h"
#include "../vk_draw.h"
#include "../gpu.h"
#include "../gal.h"
#include "../../common/log.h"

#include <algorithm>
#include <cstring>

// ===========================================================================
// VulkanDevice — thin adapter over the existing GPU::* global API.
// ===========================================================================
class VulkanDevice : public GpuDevice {
public:
    VulkanDevice() = default;
    ~VulkanDevice() override { Shutdown(); }

    // ---- lifecycle -------------------------------------------------------
    bool Initialize(const GalConfig& config,
                    const GalWindowCallbacks& callbacks) override {
        if (m_initialized) Shutdown();

        m_config = config;
        m_callbacks = callbacks;

        // Store config for later queries.
        m_swapchain_width = config.width;
        m_swapchain_height = config.height;

        // VRR / vsync config is pushed to the present layer.
        GPU::VkPresentSetVrrConfig(config.vsync, config.vrr);

        // The existing GPU::Initialize() handles Vulkan context creation,
        // window creation, and swapchain setup.
        // FIXME: once the Vulkan backend is fully refactored, this will
        // use the GAL interface directly instead of the global GPU::* API.
        m_initialized = true;
        LOG_INFO(GPU, "VulkanDevice: initialized (adapter mode, delegating to GPU::*)");
        return true;
    }

    void Shutdown() override {
        if (!m_initialized) return;
        // GPU::Shutdown() handles all Vulkan teardown.
        m_initialized = false;
    }

    GalCaps GetCaps() const override {
        GalCaps caps;
        caps.backend_name = "Vulkan (adapter)";
        caps.adapter_name = "Vulkan Physical Device";
        caps.has_compute = true;
        caps.has_tessellation = true;
        caps.max_texture_size = 16384;
        caps.dedicated_vram_bytes = 0;  // FIXME: query from VkPhysicalDeviceMemoryProperties
        caps.shared_vram_bytes = 0;
        return caps;
    }

    // ---- window / present ------------------------------------------------
    bool CreateSwapchain(int width, int height) override {
        m_swapchain_width = width;
        m_swapchain_height = height;
        // FIXME: delegate to VkPresentResize once the context is accessible.
        // Currently handled by the global GPU::Initialize().
        return GPU::HasWindow();
    }

    void DestroySwapchain() override {
        // Handled by GPU::Shutdown().
    }

    bool Present() override {
        // FIXME: delegate to GPU::RenderFrame() with current FB.
        // This requires the current framebuffer guest address, which varies
        // per frame and is set via GPU::SetFramebufferConfig().
        return true;
    }

    void ResizeSwapchain(int width, int height) override {
        m_swapchain_width = width;
        m_swapchain_height = height;
        // GPU::SetFramebufferConfig() handles this on the next RenderFrame().
    }

    void PumpEvents() override {
        GPU::PumpWindowEvents();
    }

    // ---- resource creation (stubs — real impl in vk_draw.cpp) ------------
    GalImage CreateImage(const GalImageDesc& /*desc*/) override {
        return nullptr;
    }

    void DestroyImage(GalImage /*image*/) override {}

    GalBuffer CreateBuffer(const GalBufferDesc& /*desc*/) override {
        return nullptr;
    }

    void DestroyBuffer(GalBuffer /*buffer*/) override {}

    GalShader CreateShader(const uint32_t* /*spirv_data*/,
                            size_t /*spirv_word_count*/) override {
        return nullptr;
    }

    void DestroyShader(GalShader /*shader*/) override {}

    // ---- pipeline (stubs) ------------------------------------------------
    GalPipeline CreatePipeline(GalShader /*vs*/, GalShader /*ps*/,
                                const GalBlendState& /*blend*/,
                                const GalRasterState& /*raster*/,
                                const GalDepthState& /*depth*/,
                                GalPrimitiveType /*prim*/,
                                int /*color_attachment_count*/,
                                GalFormat /*color_format*/,
                                GalFormat /*depth_format*/) override {
        return nullptr;
    }

    void DestroyPipeline(GalPipeline /*pipeline*/) override {}

    // ---- command submission (stubs) --------------------------------------
    void BeginFrame() override {}
    void EndFrame() override {}
    void Flush() override {}

    void BeginRenderPass(GalImage /*target*/) override {}
    void EndRenderPass() override {}

    void SetViewport(const GalViewport& /*vp*/) override {}
    void SetScissor(const GalScissorRect& /*scissor*/) override {}
    void SetPipeline(GalPipeline /*pipeline*/) override {}
    void SetVertexBuffer(GalBuffer /*buffer*/, uint64_t /*offset*/) override {}
    void SetIndexBuffer(GalBuffer /*buffer*/, uint64_t /*offset*/) override {}
    void SetUniformBuffer(GalBuffer /*buffer*/, uint64_t /*offset*/,
                           uint64_t /*size*/, int /*slot*/) override {}
    void Draw(int /*vertex_count*/, int /*first_vertex*/) override {}
    void DrawIndexed(int /*index_count*/, int /*first_index*/,
                      int /*vertex_offset*/) override {}

    // ---- guest memory upload helpers -------------------------------------
    void UploadImage(GalImage /*image*/, uint64_t /*guest_addr*/,
                      uint32_t /*width*/, uint32_t /*height*/,
                      GalFormat /*format*/) override {}

    void UploadBuffer(GalBuffer /*buffer*/, uint64_t /*guest_addr*/,
                       uint64_t /*size*/) override {}

    // ---- readback --------------------------------------------------------
    bool ReadbackFrame(uint8_t* /*out_pixels*/, uint32_t* /*out_width*/,
                        uint32_t* /*out_height*/) override {
        return false;
    }

    // ---- boot screen -----------------------------------------------------
    void SetBootStatus(const char* stage, int done, int total) override {
        GPU::SetBootStatus(stage, done, total);
    }

    bool IsBootActive() const override {
        return GPU::IsBootScreenActive();
    }

private:
    bool m_initialized = false;
    GalConfig m_config{};
    GalWindowCallbacks m_callbacks{};
    int m_swapchain_width = 0;
    int m_swapchain_height = 0;
};

// ===========================================================================
// Factory entry point
// ===========================================================================
GpuDevice* CreateVulkanDevice(const GalConfig& config,
                               const GalWindowCallbacks& callbacks) {
    auto* dev = new VulkanDevice();
    if (!dev->Initialize(config, callbacks)) {
        delete dev;
        return nullptr;
    }
    return dev;
}
