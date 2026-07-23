// See vk_present.h for the design notes.
#include "vk_present.h"
#include "../common/log.h"
#include <cstring>
#include <mutex>
#include <vector>

namespace GPU {

// VRR / vsync config globals — set from ConfigService by the frontend
// via VkPresentSetVrrConfig().
bool g_config_vsync = true;
bool g_config_vrr   = false;

void VkPresentSetVrrConfig(bool vsync, bool vrr) {
    g_config_vsync = vsync;
    g_config_vrr   = vrr;
}

namespace {

struct PresentState {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat       format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D     extent = {};
    std::vector<VkImage> images;

    VkCommandPool   cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore     acquire_sem = VK_NULL_HANDLE;
    VkSemaphore     done_sem = VK_NULL_HANDLE;
    VkFence         fence = VK_NULL_HANDLE;

    // Guest-FB upload resources (recreated when the FB size changes).
    VkBuffer       staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    VkDeviceSize   staging_size = 0;
    VkImage        tex = VK_NULL_HANDLE;
    VkDeviceMemory tex_mem = VK_NULL_HANDLE;
    u32            tex_w = 0;
    u32            tex_h = 0;

    // sRGB-encode intermediate for linear-float presents (recreated when the
    // swapchain extent changes).  See VkPresentFromImage.
    VkImage        encode_img = VK_NULL_HANDLE;
    VkDeviceMemory encode_mem = VK_NULL_HANDLE;
    VkExtent2D     encode_extent = {};

    // Phase 5 validation: per-frame readback hook (golden-image tests).
    // Null hook = zero cost.  The readback buffer holds one unscaled BGRA8
    // frame copied out of the presented source image.
    VkPresentReadbackFn readback_fn = nullptr;
    void*               readback_user = nullptr;
    u64                 readback_frame = 0;
    VkBuffer            rb_buf = VK_NULL_HANDLE;
    VkDeviceMemory      rb_mem = VK_NULL_HANDLE;
    VkDeviceSize        rb_size = 0;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
};

PresentState g_ps;
std::mutex   g_present_mutex;

} // namespace

VkFormat VkPresentSrgbCounterpart(VkFormat format) {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
        case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
        default: return VK_FORMAT_UNDEFINED;
    }
}

bool VkPresentIsLinearFloatSource(VkFormat format) {
    return format == VK_FORMAT_R16G16B16A16_SFLOAT ||
           format == VK_FORMAT_R32G32B32A32_SFLOAT;
}

namespace {

// Largest centered rect of aspect src_w:src_h inside the swapchain extent
// (letterbox/pillarbox).  Returned as {x0, y0, x1, y1} blit dst offsets.
void ComputeFitRect(u32 src_w, u32 src_h, s32 out[4]) {
    const u32 win_w = g_ps.extent.width;
    const u32 win_h = g_ps.extent.height;
    s32 x0 = 0, y0 = 0, x1 = static_cast<s32>(win_w), y1 = static_cast<s32>(win_h);
    if (src_w != 0 && src_h != 0 && win_w != 0 && win_h != 0) {
        // Compare src_w/src_h vs win_w/win_h cross-multiplied (no floats).
        if (static_cast<u64>(src_w) * win_h > static_cast<u64>(src_h) * win_w) {
            // Source is wider: letterbox (bars top/bottom).
            const u32 fit_h = static_cast<u32>((static_cast<u64>(win_w) * src_h) / src_w);
            y0 = static_cast<s32>((win_h - fit_h) / 2);
            y1 = y0 + static_cast<s32>(fit_h);
        } else if (static_cast<u64>(src_w) * win_h < static_cast<u64>(src_h) * win_w) {
            // Source is taller: bars left/right.
            const u32 fit_w = static_cast<u32>((static_cast<u64>(win_h) * src_w) / src_h);
            x0 = static_cast<s32>((win_w - fit_w) / 2);
            x1 = x0 + static_cast<s32>(fit_w);
        }
    }
    out[0] = x0; out[1] = y0; out[2] = x1; out[3] = y1;
}

// Clears the whole target black (letterbox bars) — the image starts in
// UNDEFINED, so without this the bars would show garbage.
void ClearBlack(VkContext* ctx, VkImage target) {
    VkClearColorValue black = {};
    VkImageSubresourceRange range = {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    ctx->fn.CmdClearColorImage(g_ps.cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               &black, 1, &range);
}

void Barrier(VkContext* ctx, VkImage image, VkImageLayout from, VkImageLayout to,
             VkAccessFlags src_access, VkAccessFlags dst_access,
             VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    ctx->fn.CmdPipelineBarrier(g_ps.cmd, src_stage, dst_stage, 0,
                               0, nullptr, 0, nullptr, 1, &b);
}

bool CreateSwapchain(VkContext* ctx, u32 width, u32 height) {
    VkSurfaceCapabilitiesKHR caps;
    if (ctx->fn.GetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->phys, ctx->surface, &caps) != VK_SUCCESS) {
        LOG_ERROR(GPU, "Vulkan present: vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed.");
        return false;
    }

    // Pick a surface format (prefer B8G8R8A8 — matches our CPU-side swizzle).
    u32 fmt_count = 0;
    ctx->fn.GetPhysicalDeviceSurfaceFormatsKHR(ctx->phys, ctx->surface, &fmt_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmt_count);
    ctx->fn.GetPhysicalDeviceSurfaceFormatsKHR(ctx->phys, ctx->surface, &fmt_count, formats.data());
    if (fmt_count == 0) {
        LOG_ERROR(GPU, "Vulkan present: no surface formats.");
        return false;
    }
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) {
            chosen = f;
            break;
        }
    }

    // R1: VRR support — query available present modes and pick the best one
    // based on the config (video.vrr, video.vsync).
    u32 pm_count = 0;
    ctx->fn.GetPhysicalDeviceSurfacePresentModesKHR(ctx->phys, ctx->surface,
                                                     &pm_count, nullptr);
    std::vector<VkPresentModeKHR> present_modes(pm_count);
    ctx->fn.GetPhysicalDeviceSurfacePresentModesKHR(ctx->phys, ctx->surface,
                                                     &pm_count,
                                                     present_modes.data());

    auto has_mode = [&](VkPresentModeKHR m) {
        for (auto pm : present_modes) if (pm == m) return true;
        return false;
    };

    // Select present mode:
    //   VRR + RELAXED available → FIFO_RELAXED (adaptive vsync with VRR)
    //   VRR + IMMEDIATE → IMMEDIATE (tear-free via VRR display)
    //   config.vsync = true → FIFO (guaranteed, traditional vsync)
    //   config.vsync = false → IMMEDIATE (uncapped, may tear)
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (g_config_vrr && has_mode(VK_PRESENT_MODE_FIFO_RELAXED_KHR)) {
        present_mode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    } else if (g_config_vrr && has_mode(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
        present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    } else if (!g_config_vsync && has_mode(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
        present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    } else if (has_mode(VK_PRESENT_MODE_MAILBOX_KHR)) {
        present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = width;
        extent.height = height;
    }
    if (extent.width == 0 || extent.height == 0) {
        LOG_WARN(GPU, "Vulkan present: zero-sized surface (minimized?) — deferring.");
        return false;
    }

    u32 image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    } else {
        LOG_ERROR(GPU, "Vulkan present: swapchain images cannot be blit/clear targets.");
        return false;
    }

    VkSwapchainCreateInfoKHR ci = {};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = ctx->surface;
    ci.minImageCount = image_count;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = usage;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = present_mode;
    ci.clipped = VK_TRUE;

    const VkResult r = ctx->fn.CreateSwapchainKHR(ctx->device, &ci, nullptr, &g_ps.swapchain);
    if (r != VK_SUCCESS) {
        LOG_ERROR(GPU, "Vulkan present: vkCreateSwapchainKHR failed (%d).", static_cast<int>(r));
        return false;
    }
    g_ps.format = chosen.format;
    g_ps.extent = extent;

    u32 count = 0;
    ctx->fn.GetSwapchainImagesKHR(ctx->device, g_ps.swapchain, &count, nullptr);
    g_ps.images.resize(count);
    ctx->fn.GetSwapchainImagesKHR(ctx->device, g_ps.swapchain, &count, g_ps.images.data());
    LOG_INFO(GPU, "Vulkan present: swapchain %ux%u, %u images, format %d.",
             extent.width, extent.height, count, static_cast<int>(chosen.format));
    return true;
}

bool CreateSyncAndCommands(VkContext* ctx) {
    VkCommandPoolCreateInfo pci = {};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx->queue_family;
    if (ctx->fn.CreateCommandPool(ctx->device, &pci, nullptr, &g_ps.cmd_pool) != VK_SUCCESS) {
        LOG_ERROR(GPU, "Vulkan present: vkCreateCommandPool failed.");
        return false;
    }

    VkCommandBufferAllocateInfo cai = {};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = g_ps.cmd_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (ctx->fn.AllocateCommandBuffers(ctx->device, &cai, &g_ps.cmd) != VK_SUCCESS) {
        LOG_ERROR(GPU, "Vulkan present: vkAllocateCommandBuffers failed.");
        return false;
    }

    VkSemaphoreCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fci = {};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (ctx->fn.CreateSemaphore(ctx->device, &sci, nullptr, &g_ps.acquire_sem) != VK_SUCCESS ||
        ctx->fn.CreateSemaphore(ctx->device, &sci, nullptr, &g_ps.done_sem) != VK_SUCCESS ||
        ctx->fn.CreateFence(ctx->device, &fci, nullptr, &g_ps.fence) != VK_SUCCESS) {
        LOG_ERROR(GPU, "Vulkan present: sync object creation failed.");
        return false;
    }
    return true;
}

// (Re)creates the staging buffer + guest-FB texture for fb_w x fb_h.
bool EnsureUploadResources(VkContext* ctx, u32 fb_w, u32 fb_h) {
    const VkDeviceSize need = static_cast<VkDeviceSize>(fb_w) * fb_h * 4;
    if (g_ps.tex != VK_NULL_HANDLE && g_ps.tex_w == fb_w && g_ps.tex_h == fb_h &&
        g_ps.staging_size >= need) {
        return true;
    }

    if (g_ps.staging) ctx->fn.DestroyBuffer(ctx->device, g_ps.staging, nullptr);
    if (g_ps.staging_mem) ctx->fn.FreeMemory(ctx->device, g_ps.staging_mem, nullptr);
    if (g_ps.tex) ctx->fn.DestroyImage(ctx->device, g_ps.tex, nullptr);
    if (g_ps.tex_mem) ctx->fn.FreeMemory(ctx->device, g_ps.tex_mem, nullptr);
    g_ps.staging = VK_NULL_HANDLE; g_ps.staging_mem = VK_NULL_HANDLE;
    g_ps.tex = VK_NULL_HANDLE; g_ps.tex_mem = VK_NULL_HANDLE;

    // Staging buffer.
    VkBufferCreateInfo bci = {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = need;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (ctx->fn.CreateBuffer(ctx->device, &bci, nullptr, &g_ps.staging) != VK_SUCCESS) {
        LOG_ERROR(GPU, "Vulkan present: staging buffer creation failed.");
        return false;
    }
    VkMemoryRequirements req;
    ctx->fn.GetBufferMemoryRequirements(ctx->device, g_ps.staging, &req);
    u32 type = 0;
    if (!VkFindMemoryType(ctx, req.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &type)) {
        LOG_ERROR(GPU, "Vulkan present: no host-visible memory type for staging.");
        return false;
    }
    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (ctx->fn.AllocateMemory(ctx->device, &mai, nullptr, &g_ps.staging_mem) != VK_SUCCESS ||
        ctx->fn.BindBufferMemory(ctx->device, g_ps.staging, g_ps.staging_mem, 0) != VK_SUCCESS) {
        LOG_ERROR(GPU, "Vulkan present: staging memory allocation failed.");
        return false;
    }
    g_ps.staging_size = need;

    // Guest-FB texture.
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_B8G8R8A8_UNORM;
    ici.extent = { fb_w, fb_h, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (ctx->fn.CreateImage(ctx->device, &ici, nullptr, &g_ps.tex) != VK_SUCCESS) {
        LOG_ERROR(GPU, "Vulkan present: guest-FB texture creation failed.");
        return false;
    }
    ctx->fn.GetImageMemoryRequirements(ctx->device, g_ps.tex, &req);
    if (!VkFindMemoryType(ctx, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
        LOG_ERROR(GPU, "Vulkan present: no device-local memory type for texture.");
        return false;
    }
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (ctx->fn.AllocateMemory(ctx->device, &mai, nullptr, &g_ps.tex_mem) != VK_SUCCESS ||
        ctx->fn.BindImageMemory(ctx->device, g_ps.tex, g_ps.tex_mem, 0) != VK_SUCCESS) {
        LOG_ERROR(GPU, "Vulkan present: texture memory allocation failed.");
        return false;
    }
    g_ps.tex_w = fb_w;
    g_ps.tex_h = fb_h;
    LOG_INFO(GPU, "Vulkan present: upload resources for %ux%u guest framebuffer.", fb_w, fb_h);
    return true;
}

void DestroyPresentEncodeImage(VkContext* ctx);

// PS5 float VideoOut buffers (A16B16G16R16F flips) hold linear scRGB light
// where 1.0 is SDR white; hardware scan-out applies the display transfer.
// vkCmdBlitImage converts numerically only, so presenting a linear-float
// guest frame into a UNORM swapchain shows near-black for any dim scene.
// Encode linear->sRGB by blitting through an sRGB intermediate (sRGB stores
// encode), then raw-copying the encoded bytes into the same-class UNORM
// swapchain image.  (SharpEmu VulkanVideoPresenter #448.)
//
// (Re)creates the swapchain-sized sRGB intermediate.  Returns false when the
// swapchain format has no sRGB counterpart — the raw blit stays.
bool TryGetPresentEncodeImage(VkContext* ctx, VkImage* out) {
    const VkFormat encode_format = VkPresentSrgbCounterpart(g_ps.format);
    if (encode_format == VK_FORMAT_UNDEFINED) return false;

    if (g_ps.encode_img != VK_NULL_HANDLE &&
        (g_ps.encode_extent.width != g_ps.extent.width ||
         g_ps.encode_extent.height != g_ps.extent.height)) {
        DestroyPresentEncodeImage(ctx);
    }

    if (g_ps.encode_img == VK_NULL_HANDLE) {
        VkImageCreateInfo ici = {};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = encode_format;
        ici.extent = { g_ps.extent.width, g_ps.extent.height, 1 };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (ctx->fn.CreateImage(ctx->device, &ici, nullptr, &g_ps.encode_img) != VK_SUCCESS) {
            LOG_WARN(GPU, "Vulkan present: sRGB-encode image creation failed.");
            return false;
        }
        VkMemoryRequirements req;
        ctx->fn.GetImageMemoryRequirements(ctx->device, g_ps.encode_img, &req);
        u32 type = 0;
        if (!VkFindMemoryType(ctx, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
            LOG_WARN(GPU, "Vulkan present: no device-local memory type for encode image.");
            ctx->fn.DestroyImage(ctx->device, g_ps.encode_img, nullptr);
            g_ps.encode_img = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = type;
        if (ctx->fn.AllocateMemory(ctx->device, &mai, nullptr, &g_ps.encode_mem) != VK_SUCCESS ||
            ctx->fn.BindImageMemory(ctx->device, g_ps.encode_img, g_ps.encode_mem, 0) != VK_SUCCESS) {
            LOG_WARN(GPU, "Vulkan present: encode image memory allocation failed.");
            if (g_ps.encode_mem) ctx->fn.FreeMemory(ctx->device, g_ps.encode_mem, nullptr);
            g_ps.encode_mem = VK_NULL_HANDLE;
            ctx->fn.DestroyImage(ctx->device, g_ps.encode_img, nullptr);
            g_ps.encode_img = VK_NULL_HANDLE;
            return false;
        }
        g_ps.encode_extent = g_ps.extent;
        LOG_INFO(GPU, "Vulkan present: sRGB-encode image %ux%u (format %d).",
                 g_ps.extent.width, g_ps.extent.height, static_cast<int>(encode_format));
    }

    *out = g_ps.encode_img;
    return true;
}

void DestroyPresentEncodeImage(VkContext* ctx) {
    if (g_ps.encode_img) ctx->fn.DestroyImage(ctx->device, g_ps.encode_img, nullptr);
    if (g_ps.encode_mem) ctx->fn.FreeMemory(ctx->device, g_ps.encode_mem, nullptr);
    g_ps.encode_img = VK_NULL_HANDLE;
    g_ps.encode_mem = VK_NULL_HANDLE;
    g_ps.encode_extent = {};
}

// Acquires an image, records `record`, submits and presents.  Returns false on
// failure (caller may fall back to GDI for this frame).
template <typename F>
bool AcquireRecordPresent(VkContext* ctx, F&& record) {
    u32 index = 0;
    const VkResult acq = ctx->fn.AcquireNextImageKHR(ctx->device, g_ps.swapchain, UINT64_MAX,
                                                     g_ps.acquire_sem, VK_NULL_HANDLE, &index);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        return false;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        LOG_WARN(GPU, "Vulkan present: vkAcquireNextImageKHR failed (%d).", static_cast<int>(acq));
        return false;
    }

    ctx->fn.WaitForFences(ctx->device, 1, &g_ps.fence, VK_TRUE, UINT64_MAX);
    ctx->fn.ResetFences(ctx->device, 1, &g_ps.fence);
    ctx->fn.ResetCommandBuffer(g_ps.cmd, 0);

    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ctx->fn.BeginCommandBuffer(g_ps.cmd, &bi);
    record(g_ps.images[index]);
    ctx->fn.EndCommandBuffer(g_ps.cmd);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g_ps.acquire_sem;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_ps.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g_ps.done_sem;
    if (ctx->fn.QueueSubmit(ctx->queue, 1, &si, g_ps.fence) != VK_SUCCESS) {
        LOG_WARN(GPU, "Vulkan present: vkQueueSubmit failed.");
        return false;
    }

    VkPresentInfoKHR pi = {};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g_ps.done_sem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_ps.swapchain;
    pi.pImageIndices = &index;
    const VkResult pr = ctx->fn.QueuePresentKHR(ctx->queue, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR) {
        return false;
    }
    if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
        LOG_WARN(GPU, "Vulkan present: vkQueuePresentKHR failed (%d).", static_cast<int>(pr));
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Phase 5 validation: frame readback (golden-image tests).
// ---------------------------------------------------------------------------

void VkPresentSetReadbackHook(VkPresentReadbackFn fn, void* user) {
    std::lock_guard<std::mutex> lk(g_present_mutex);
    g_ps.readback_fn = fn;
    g_ps.readback_user = user;
    g_ps.readback_frame = 0;
}

namespace {

// (Re)creates the host-visible readback buffer for w x h BGRA8.
bool EnsureReadbackBuffer(VkContext* ctx, u32 w, u32 h) {
    const VkDeviceSize need = static_cast<VkDeviceSize>(w) * h * 4;
    if (g_ps.rb_buf != VK_NULL_HANDLE && g_ps.rb_size >= need) return true;
    if (g_ps.rb_buf) ctx->fn.DestroyBuffer(ctx->device, g_ps.rb_buf, nullptr);
    if (g_ps.rb_mem) ctx->fn.FreeMemory(ctx->device, g_ps.rb_mem, nullptr);
    g_ps.rb_buf = VK_NULL_HANDLE;
    g_ps.rb_mem = VK_NULL_HANDLE;

    VkBufferCreateInfo bci = {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = need;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (ctx->fn.CreateBuffer(ctx->device, &bci, nullptr, &g_ps.rb_buf) != VK_SUCCESS) {
        LOG_WARN(GPU, "Vulkan present readback: buffer creation failed.");
        return false;
    }
    VkMemoryRequirements req;
    ctx->fn.GetBufferMemoryRequirements(ctx->device, g_ps.rb_buf, &req);
    u32 type = 0;
    if (!VkFindMemoryType(ctx, req.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &type)) {
        LOG_WARN(GPU, "Vulkan present readback: no host-visible memory type.");
        return false;
    }
    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (ctx->fn.AllocateMemory(ctx->device, &mai, nullptr, &g_ps.rb_mem) != VK_SUCCESS ||
        ctx->fn.BindBufferMemory(ctx->device, g_ps.rb_buf, g_ps.rb_mem, 0) != VK_SUCCESS) {
        LOG_WARN(GPU, "Vulkan present readback: memory allocation failed.");
        return false;
    }
    g_ps.rb_size = need;
    return true;
}

// Records a copy of `image` (must currently be in TRANSFER_SRC_OPTIMAL) into
// the readback buffer.  Caller holds g_present_mutex and is inside the
// present command buffer.
void RecordReadbackCopy(VkContext* ctx, VkImage image, u32 w, u32 h) {
    if (g_ps.CmdCopyImageToBuffer == nullptr) {
        g_ps.CmdCopyImageToBuffer = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(
            ctx->fn.GetDeviceProcAddr(ctx->device, "vkCmdCopyImageToBuffer"));
        if (g_ps.CmdCopyImageToBuffer == nullptr) {
            LOG_WARN(GPU, "Vulkan present readback: vkCmdCopyImageToBuffer unavailable.");
            return;
        }
    }
    if (!EnsureReadbackBuffer(ctx, w, h)) return;
    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;   // tightly packed
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { w, h, 1 };
    g_ps.CmdCopyImageToBuffer(g_ps.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              g_ps.rb_buf, 1, &region);
}

// Waits for the just-submitted present work, maps the readback buffer and
// delivers the frame to the hook.  Caller holds g_present_mutex.
void DeliverReadback(VkContext* ctx, u32 w, u32 h) {
    if (g_ps.rb_buf == VK_NULL_HANDLE) return;
    ctx->fn.WaitForFences(ctx->device, 1, &g_ps.fence, VK_TRUE, UINT64_MAX);
    void* mapped = nullptr;
    if (ctx->fn.MapMemory(ctx->device, g_ps.rb_mem, 0, static_cast<VkDeviceSize>(w) * h * 4,
                          0, &mapped) != VK_SUCCESS) {
        LOG_WARN(GPU, "Vulkan present readback: map failed.");
        return;
    }
    g_ps.readback_fn(reinterpret_cast<const u8*>(mapped), w, h,
                     g_ps.readback_frame++, g_ps.readback_user);
    ctx->fn.UnmapMemory(ctx->device, g_ps.rb_mem);
}

} // namespace

bool VkPresentInitialize(VkContext* ctx, u32 width, u32 height) {
    std::lock_guard<std::mutex> lk(g_present_mutex);
    if (!CreateSwapchain(ctx, width, height)) return false;
    if (!CreateSyncAndCommands(ctx)) return false;
    LOG_INFO(GPU, "Vulkan present path initialized (FIFO vsync).");
    return true;
}

bool VkPresentIsReady() {
    return g_ps.swapchain != VK_NULL_HANDLE;
}

bool VkPresentResize(VkContext* ctx, u32 width, u32 height) {
    if (!ctx || !ctx->device || width == 0 || height == 0) return false;
    std::lock_guard<std::mutex> lk(g_present_mutex);
    if (g_ps.swapchain == VK_NULL_HANDLE) return false;
    if (g_ps.extent.width == width && g_ps.extent.height == height) return true;
    ctx->fn.DeviceWaitIdle(ctx->device);
    ctx->fn.DestroySwapchainKHR(ctx->device, g_ps.swapchain, nullptr);
    g_ps.swapchain = VK_NULL_HANDLE;
    g_ps.images.clear();
    if (!CreateSwapchain(ctx, width, height)) {
        LOG_WARN(GPU, "Vulkan present: swapchain recreate failed (%ux%u) — GDI fallback.", width, height);
        return false;
    }
    LOG_INFO(GPU, "Vulkan present: swapchain recreated for %ux%u.", width, height);
    return true;
}

void VkPresentShutdown(VkContext* ctx) {
    std::lock_guard<std::mutex> lk(g_present_mutex);
    if (!ctx || !ctx->device) return;
    ctx->fn.DeviceWaitIdle(ctx->device);
    if (g_ps.staging) ctx->fn.DestroyBuffer(ctx->device, g_ps.staging, nullptr);
    if (g_ps.staging_mem) ctx->fn.FreeMemory(ctx->device, g_ps.staging_mem, nullptr);
    if (g_ps.tex) ctx->fn.DestroyImage(ctx->device, g_ps.tex, nullptr);
    if (g_ps.tex_mem) ctx->fn.FreeMemory(ctx->device, g_ps.tex_mem, nullptr);
    if (g_ps.acquire_sem) ctx->fn.DestroySemaphore(ctx->device, g_ps.acquire_sem, nullptr);
    if (g_ps.done_sem) ctx->fn.DestroySemaphore(ctx->device, g_ps.done_sem, nullptr);
    if (g_ps.fence) ctx->fn.DestroyFence(ctx->device, g_ps.fence, nullptr);
    if (g_ps.cmd_pool) ctx->fn.DestroyCommandPool(ctx->device, g_ps.cmd_pool, nullptr);
    if (g_ps.rb_buf) ctx->fn.DestroyBuffer(ctx->device, g_ps.rb_buf, nullptr);
    if (g_ps.rb_mem) ctx->fn.FreeMemory(ctx->device, g_ps.rb_mem, nullptr);
    DestroyPresentEncodeImage(ctx);
    if (g_ps.swapchain) ctx->fn.DestroySwapchainKHR(ctx->device, g_ps.swapchain, nullptr);
    g_ps = PresentState{};
}

bool VkPresentFrame(VkContext* ctx, const void* bgra_pixels, u32 fb_w, u32 fb_h) {
    if (!ctx || !bgra_pixels || fb_w == 0 || fb_h == 0) return false;
    std::lock_guard<std::mutex> lk(g_present_mutex);
    if (g_ps.swapchain == VK_NULL_HANDLE) return false;
    if (!EnsureUploadResources(ctx, fb_w, fb_h)) return false;

    void* mapped = nullptr;
    if (ctx->fn.MapMemory(ctx->device, g_ps.staging_mem, 0, g_ps.staging_size, 0, &mapped) != VK_SUCCESS) {
        LOG_WARN(GPU, "Vulkan present: staging map failed.");
        return false;
    }
    memcpy(mapped, bgra_pixels, static_cast<size_t>(fb_w) * fb_h * 4);
    ctx->fn.UnmapMemory(ctx->device, g_ps.staging_mem);

    const bool presented = AcquireRecordPresent(ctx, [&](VkImage target) {
        // Texture: UNDEFINED -> TRANSFER_DST, copy, -> TRANSFER_SRC.
        Barrier(ctx, g_ps.tex, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region = {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = { fb_w, fb_h, 1 };
        ctx->fn.CmdCopyBufferToImage(g_ps.cmd, g_ps.staging, g_ps.tex,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        Barrier(ctx, g_ps.tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        // Phase 5 validation: read back the uploaded guest FB (pre-scale).
        if (g_ps.readback_fn != nullptr) {
            RecordReadbackCopy(ctx, g_ps.tex, fb_w, fb_h);
        }

        // Swapchain image: UNDEFINED -> TRANSFER_DST, blit scaled, -> PRESENT_SRC.
        Barrier(ctx, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit blit = {};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1] = { static_cast<s32>(fb_w), static_cast<s32>(fb_h), 1 };
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        s32 fit[4];
        ComputeFitRect(fb_w, fb_h, fit);
        blit.dstOffsets[0] = { fit[0], fit[1], 0 };
        blit.dstOffsets[1] = { fit[2], fit[3], 1 };
        ClearBlack(ctx, target);
        ctx->fn.CmdBlitImage(g_ps.cmd, g_ps.tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                             VK_FILTER_LINEAR);
        Barrier(ctx, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    });
    if (presented && g_ps.readback_fn != nullptr) {
        DeliverReadback(ctx, fb_w, fb_h);
    }
    return presented;
}

bool VkPresentFromImage(VkContext* ctx, VkImage src, VkFormat src_format,
                        u32 src_w, u32 src_h) {
    if (!ctx || src == VK_NULL_HANDLE || src_w == 0 || src_h == 0) return false;
    std::lock_guard<std::mutex> lk(g_present_mutex);
    if (g_ps.swapchain == VK_NULL_HANDLE) return false;

    // Linear-float flips need a linear->sRGB encode on the way to a UNORM
    // swapchain; sRGB (or unknown-counterpart) swapchains keep the direct
    // blit.  (SharpEmu VulkanVideoPresenter #448.)
    VkImage encode_img = VK_NULL_HANDLE;
    const bool encode_for_present =
        VkPresentIsLinearFloatSource(src_format) &&
        VkPresentSrgbCounterpart(g_ps.format) != VK_FORMAT_UNDEFINED &&
        TryGetPresentEncodeImage(ctx, &encode_img);

    const bool presented = AcquireRecordPresent(ctx, [&](VkImage target) {
        // Source render target: COLOR_ATTACHMENT -> TRANSFER_SRC.
        Barrier(ctx, src, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);

        // Phase 5 validation: read back the presented render target.
        if (g_ps.readback_fn != nullptr) {
            RecordReadbackCopy(ctx, src, src_w, src_h);
        }

        // Swapchain image: UNDEFINED -> TRANSFER_DST, blit scaled, -> PRESENT_SRC.
        Barrier(ctx, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        // When encoding, the sRGB intermediate is the blit destination; the
        // copy below lands the encoded bytes in the swapchain image.
        const VkImage blit_dst = encode_for_present ? encode_img : target;
        if (encode_for_present) {
            Barrier(ctx, encode_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
        VkImageBlit blit = {};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1] = { static_cast<s32>(src_w), static_cast<s32>(src_h), 1 };
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        s32 fit[4];
        ComputeFitRect(src_w, src_h, fit);
        blit.dstOffsets[0] = { fit[0], fit[1], 0 };
        blit.dstOffsets[1] = { fit[2], fit[3], 1 };
        ClearBlack(ctx, blit_dst);
        ctx->fn.CmdBlitImage(g_ps.cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             blit_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                             VK_FILTER_LINEAR);

        if (encode_for_present) {
            Barrier(ctx, encode_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

            // Raw same-class copy keeps the sRGB-encoded bytes unchanged
            // while landing them in the UNORM swapchain image.
            VkImageCopy copy = {};
            copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.srcSubresource.layerCount = 1;
            copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.dstSubresource.layerCount = 1;
            copy.extent = { g_ps.extent.width, g_ps.extent.height, 1 };
            ctx->fn.CmdCopyImage(g_ps.cmd, encode_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        }

        Barrier(ctx, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        // Restore the render-target layout for the next guest draw.
        Barrier(ctx, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    });
    if (presented && g_ps.readback_fn != nullptr) {
        DeliverReadback(ctx, src_w, src_h);
    }
    return presented;
}

bool VkPresentClearColor(VkContext* ctx, float r, float g, float b, float a) {
    if (!ctx) return false;
    std::lock_guard<std::mutex> lk(g_present_mutex);
    if (g_ps.swapchain == VK_NULL_HANDLE) return false;

    const bool presented = AcquireRecordPresent(ctx, [&](VkImage target) {
        Barrier(ctx, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue color = {};
        color.float32[0] = r;
        color.float32[1] = g;
        color.float32[2] = b;
        color.float32[3] = a;
        VkImageSubresourceRange range = {};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        ctx->fn.CmdClearColorImage(g_ps.cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   &color, 1, &range);
        Barrier(ctx, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    });
    if (presented && g_ps.readback_fn != nullptr) {
        // Solid clear: synthesize the frame CPU-side at the swapchain extent
        // (the whole image is one color by definition).
        const u32 w = g_ps.extent.width, h = g_ps.extent.height;
        u32 px = 0xFF000000u;
        px |= (static_cast<u32>(r * 255.0f + 0.5f) & 0xFFu) << 16;
        px |= (static_cast<u32>(g * 255.0f + 0.5f) & 0xFFu) << 8;
        px |= (static_cast<u32>(b * 255.0f + 0.5f) & 0xFFu);
        std::vector<u8> frame(static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i + 4 <= frame.size(); i += 4) {
            std::memcpy(frame.data() + i, &px, 4);
        }
        g_ps.readback_fn(frame.data(), w, h, g_ps.readback_frame++, g_ps.readback_user);
    }
    return presented;
}

} // namespace GPU
