// Graphics Abstraction Layer (GAL) — abstract interface for GPU backends.
//
// Every rendering backend (Vulkan, D3D12, GDI/software, null) implements
// this interface.  The rest of the emulator accesses the GPU exclusively
// through these types; backend-specific headers (vk_*.h, d3d12_*.h, ...)
// are never included outside their own implementation files.
//
// The interface is designed for the narrow use-case of a PS5 emulator:
// - Backend-agnostic window creation + swapchain presentation
// - Guest framebuffer upload (host-visible linear textures)
// - GCN-style draw calls via translated shaders
// - Query/timestamp for diagnostics
//
// Lifetime: one GpuDevice instance lives for the duration of emulation.
// Initialize() is called once after the window system is ready;
// Shutdown() tears down every resource.
//
// Thread safety: Initialize() / Shutdown() are called from the main
// thread.  All other methods are called from the guest worker thread
// EXCEPT PumpEvents() which is main-thread-only.

#pragma once
#include "../common/types.h"
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

// ---------------------------------------------------------------------------
// Forward declarations (backend-agnostic handles)
// ---------------------------------------------------------------------------
using GalSwapchain = void*;    // opaque swapchain handle
using GalImage     = void*;    // opaque image/texture handle
using GalBuffer    = void*;    // opaque buffer handle
using GalShader    = void*;    // opaque compiled shader handle
using GalPipeline  = void*;    // opaque pipeline state object
using GalCommandBuffer = void*; // opaque command buffer

// ---------------------------------------------------------------------------
// Capability flags — queried after Initialize()
// ---------------------------------------------------------------------------
struct GalCaps {
    bool has_compute        = false;
    bool has_tessellation   = false;
    bool has_geometry_shaders = false;
    bool has_bindless_textures = false;
    bool has_conservative_raster = false;
    bool has_variable_rate_shading = false;
    bool has_ray_tracing    = false;

    // Limits
    uint32_t max_texture_size     = 0;
    uint32_t max_compute_threads  = 0;
    uint64_t dedicated_vram_bytes = 0;
    uint64_t shared_vram_bytes    = 0;

    std::string adapter_name;       // "NVIDIA GeForce RTX 4070", etc.
    std::string driver_version;     // vendor driver string
    std::string backend_name;       // "Vulkan 1.3", "D3D12", "GDI", "Null"
};

// ---------------------------------------------------------------------------
// Backend selection / configuration
// ---------------------------------------------------------------------------
enum class GalBackendType : int {
    Auto     = -1,   // probe: Vulkan → D3D12 → GDI
    Vulkan   = 0,    // existing Vulkan backend
    D3D12    = 1,    // Direct3D 12 (future)
    OpenGL   = 2,    // OpenGL 4.x (future)
    GDI      = 3,    // GDI DIB software fallback
    Null     = 4,    // headless / testing
};

struct GalConfig {
    GalBackendType backend     = GalBackendType::Auto;
    int            width       = 1280;
    int            height      = 720;
    bool           fullscreen  = false;
    float          resolution_scale = 1.0f;  // 0.5..2.0
    int            swapchain_image_count = 3;
    bool           vsync       = true;
    bool           debug       = false;      // enable validation layers

    // Window system integration (see below)
    void*          native_window_handle = nullptr;  // HWND, etc.
};

// ---------------------------------------------------------------------------
// Window system integration
// ---------------------------------------------------------------------------
// Backends create their own window (GLFW, SDL window) OR attach to a
// native window handle provided by the host (WPF embedding).  The
// callbacks are set before Initialize().
struct GalWindowCallbacks {
    // Called when the backend creates a native window.  For embedded mode
    // (WPF UI), the launcher reparents this window into its own frame.
    // `native_handle` is platform-specific (HWND on Windows).
    using CreatedCallback = void (*)(uint64_t native_handle, void* user);
    CreatedCallback on_window_created = nullptr;
    void*          on_window_created_user = nullptr;

    // Called every frame (from the main thread) to pump OS events.
    // Backend calls this from its own event loop or the host calls it.
    std::function<void()> pump_events;
};

// ---------------------------------------------------------------------------
// Resource creation hints
// ---------------------------------------------------------------------------
enum class GalFormat : int {
    // Color formats
    B8G8R8A8_UNORM       = 0,
    R8G8B8A8_UNORM       = 1,
    R8G8B8A8_SRGB        = 2,
    R16G16B16A16_SFLOAT  = 3,
    R32G32B32A32_SFLOAT  = 4,
    R32G32_SFLOAT        = 5,
    R32_SFLOAT           = 6,
    R8_UNORM             = 7,

    // Depth/stencil
    D32_SFLOAT           = 8,
    D32_SFLOAT_S8_UINT   = 9,
    D24_UNORM_S8_UINT    = 10,

    // BC compressed (guest-native)
    BC1_RGBA_UNORM       = 11,
    BC2_RGBA_UNORM       = 12,
    BC3_RGBA_UNORM       = 13,
    BC4_UNORM            = 14,
    BC5_UNORM            = 15,
    BC7_UNORM            = 16,

    Unknown              = -1,
};

enum class GalImageUsage : int {
    ColorAttachment  = 1,
    DepthAttachment  = 2,
    Sampled          = 4,
    Storage          = 8,
    TransferSrc      = 16,
    TransferDst      = 32,
};

enum class GalBufferUsage : int {
    Vertex   = 1,
    Index    = 2,
    Uniform  = 4,
    Storage  = 8,
    Indirect = 16,
};

struct GalImageDesc {
    uint32_t    width        = 0;
    uint32_t    height       = 0;
    uint32_t    depth        = 1;
    uint32_t    mip_levels   = 1;
    GalFormat   format       = GalFormat::Unknown;
    int         usage        = 0;               // bitmask of GalImageUsage
    bool        host_visible = false;            // CPU-mappable
};

struct GalBufferDesc {
    uint64_t       size  = 0;
    int            usage = 0;                    // bitmask of GalBufferUsage
    bool           host_visible = true;          // CPU-mappable
};

// ---------------------------------------------------------------------------
// Draw state
// ---------------------------------------------------------------------------
enum class GalPrimitiveType : int {
    PointList     = 0,
    LineList      = 1,
    TriangleList  = 2,
    TriangleStrip = 3,
};

struct GalViewport {
    float x = 0.0f, y = 0.0f;
    float width = 0.0f, height = 0.0f;
    float min_depth = 0.0f, max_depth = 1.0f;
};

struct GalScissorRect {
    int32_t x = 0, y = 0;
    int32_t width = 0, height = 0;
};

struct GalBlendState {
    bool   enable = false;
    int    src_color = 1;  // VK_BLEND_FACTOR_SRC_ALPHA
    int    dst_color = 0;  // VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
    int    src_alpha = 1;
    int    dst_alpha = 0;
};

struct GalRasterState {
    bool   depth_clip = true;
    bool   cull_enable = false;
    int    cull_mode = 0;     // 0=none, 1=front, 2=back
    int    fill_mode = 0;     // 0=fill, 1=line, 2=point
};

struct GalDepthState {
    bool   test_enable = false;
    bool   write_enable = true;
    int    compare_op = 0;    // 0=less, 1=equal, 2=less_equal, ...
};

// ---------------------------------------------------------------------------
// GpuDevice interface — every backend implements this
// ---------------------------------------------------------------------------
class GpuDevice {
public:
    virtual ~GpuDevice() = default;

    // ---- lifecycle -------------------------------------------------------
    virtual bool Initialize(const GalConfig& config,
                            const GalWindowCallbacks& callbacks) = 0;
    virtual void Shutdown() = 0;
    virtual GalCaps GetCaps() const = 0;

    // ---- window / present ------------------------------------------------
    virtual bool CreateSwapchain(int width, int height) = 0;
    virtual void DestroySwapchain() = 0;
    virtual bool Present() = 0;
    virtual void ResizeSwapchain(int width, int height) = 0;
    virtual void PumpEvents() = 0;    // main-thread-only

    // ---- resource creation -----------------------------------------------
    virtual GalImage CreateImage(const GalImageDesc& desc) = 0;
    virtual void DestroyImage(GalImage image) = 0;
    virtual GalBuffer CreateBuffer(const GalBufferDesc& desc) = 0;
    virtual void DestroyBuffer(GalBuffer buffer) = 0;
    virtual GalShader CreateShader(const uint32_t* spirv_data,
                                    size_t spirv_word_count) = 0;
    virtual void DestroyShader(GalShader shader) = 0;

    // ---- pipeline --------------------------------------------------------
    // Creates a graphics pipeline from a compiled vertex + fragment shader
    // and the current draw state.  The pipeline is cached internally so
    // this is safe to call every draw.
    virtual GalPipeline CreatePipeline(
        GalShader vs, GalShader ps,
        const GalBlendState& blend,
        const GalRasterState& raster,
        const GalDepthState& depth,
        GalPrimitiveType prim,
        int color_attachment_count = 1,
        GalFormat color_format = GalFormat::B8G8R8A8_UNORM,
        GalFormat depth_format = GalFormat::Unknown) = 0;
    virtual void DestroyPipeline(GalPipeline pipeline) = 0;

    // ---- command submission ----------------------------------------------
    // All draw calls are recorded into the current command buffer and
    // flushed on Present() or Flush().  One active command buffer at a
    // time.
    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;
    virtual void Flush() = 0;          // submit + wait idle
    virtual void BeginRenderPass(GalImage target) = 0;
    virtual void EndRenderPass() = 0;

    virtual void SetViewport(const GalViewport& vp) = 0;
    virtual void SetScissor(const GalScissorRect& scissor) = 0;
    virtual void SetPipeline(GalPipeline pipeline) = 0;
    virtual void SetVertexBuffer(GalBuffer buffer, uint64_t offset) = 0;
    virtual void SetIndexBuffer(GalBuffer buffer, uint64_t offset) = 0;
    virtual void SetUniformBuffer(GalBuffer buffer, uint64_t offset,
                                   uint64_t size, int slot) = 0;

    virtual void Draw(int vertex_count, int first_vertex) = 0;
    virtual void DrawIndexed(int index_count, int first_index,
                              int vertex_offset) = 0;

    // ---- guest memory upload helpers -------------------------------------
    // Upload a linear guest framebuffer (or texture) to a GPU image.
    // `guest_addr` is a direct-mapped pointer in the emulator's address
    // space.  The backend may copy immediately or defer to a staging
    // queue (the caller must not overwrite `guest_addr` until Flush()).
    virtual void UploadImage(GalImage image, uint64_t guest_addr,
                              uint32_t width, uint32_t height,
                              GalFormat format) = 0;
    virtual void UploadBuffer(GalBuffer buffer, uint64_t guest_addr,
                               uint64_t size) = 0;

    // ---- readback (diagnostics / golden frame testing) -------------------
    // Read the current swapchain image back to host memory.  Returns
    // false if the backend doesn't support readback.  The pixel data is
    // B8G8R8A8_UNORM, row-major, width*height*4 bytes.  Used by the
    // golden-frame test suite (H4.4).
    virtual bool ReadbackFrame(uint8_t* out_pixels, uint32_t* out_width,
                                uint32_t* out_height) = 0;

    // ---- boot status overlay (GDI fallback) ------------------------------
    // Before the first guest frame, the emulator shows a boot-progress
    // screen.  The backend renders this via the simplest possible path
    // (GDI blit on Windows, software compositing elsewhere) without
    // requiring a full swapchain.
    virtual void SetBootStatus(const char* stage, int done = -1,
                                int total = -1) = 0;
    virtual bool IsBootActive() const = 0;

    // ---- factory method --------------------------------------------------
    // Creates the best available backend for `type`, or nullptr if no
    // backend could be initialized (caller should try a lower-tier type).
    // The two-argument overload uses default config and window callbacks.
    static GpuDevice* Create(GalBackendType type);
    static GpuDevice* Create(GalBackendType type,
                             const GalConfig& config,
                             const GalWindowCallbacks& callbacks);
};

// ---------------------------------------------------------------------------
// Null / headless device for unit testing
// ---------------------------------------------------------------------------
class NullGpuDevice : public GpuDevice {
public:
    bool Initialize(const GalConfig&, const GalWindowCallbacks&) override { return true; }
    void Shutdown() override {}
    GalCaps GetCaps() const override { return GalCaps{}; }
    bool CreateSwapchain(int, int) override { return true; }
    void DestroySwapchain() override {}
    bool Present() override { return true; }
    void ResizeSwapchain(int, int) override {}
    void PumpEvents() override {}
    GalImage CreateImage(const GalImageDesc&) override { return nullptr; }
    void DestroyImage(GalImage) override {}
    GalBuffer CreateBuffer(const GalBufferDesc&) override { return nullptr; }
    void DestroyBuffer(GalBuffer) override {}
    GalShader CreateShader(const uint32_t*, size_t) override { return nullptr; }
    void DestroyShader(GalShader) override {}
    GalPipeline CreatePipeline(GalShader, GalShader, const GalBlendState&,
                                const GalRasterState&, const GalDepthState&,
                                GalPrimitiveType, int, GalFormat, GalFormat) override { return nullptr; }
    void DestroyPipeline(GalPipeline) override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Flush() override {}
    void BeginRenderPass(GalImage) override {}
    void EndRenderPass() override {}
    void SetViewport(const GalViewport&) override {}
    void SetScissor(const GalScissorRect&) override {}
    void SetPipeline(GalPipeline) override {}
    void SetVertexBuffer(GalBuffer, uint64_t) override {}
    void SetIndexBuffer(GalBuffer, uint64_t) override {}
    void SetUniformBuffer(GalBuffer, uint64_t, uint64_t, int) override {}
    void Draw(int, int) override {}
    void DrawIndexed(int, int, int) override {}
    void UploadImage(GalImage, uint64_t, uint32_t, uint32_t, GalFormat) override {}
    void UploadBuffer(GalBuffer, uint64_t, uint64_t) override {}
    bool ReadbackFrame(uint8_t*, uint32_t*, uint32_t*) override { return false; }
    void SetBootStatus(const char*, int, int) override {}
    bool IsBootActive() const override { return false; }
};
