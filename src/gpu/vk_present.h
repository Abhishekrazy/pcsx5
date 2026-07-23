// Swapchain + present path (Phase 5 M1).
//
// Shader-free presentation: the guest framebuffer (converted CPU-side to
// BGRA8) is uploaded through a staging buffer into a tiled texture, then
// vkCmdBlitImage scales it into the acquired swapchain image (FIFO vsync).
// No SPIR-V, no render pass — chosen over a fullscreen-triangle pipeline
// because the project has no offline shader compiler (no glslang); the blit
// path is fixed-function everywhere a desktop swapchain exists.
//
// Also provides VkPresentClearColor for the AGC PM4 clear hook: clears the
// swapchain image with vkCmdClearColorImage and presents it.
#pragma once
#include "../common/types.h"
#include "vk_context.h"

namespace GPU {

// Creates the swapchain for the current window client size.  Returns false
// (with a log line) on failure — caller stays on the GDI fallback.
bool VkPresentInitialize(VkContext* ctx, u32 width, u32 height);
// Recreates the swapchain for a new window size (fullscreen toggle, resize).
// Returns false on failure — caller drops to the GDI fallback.
bool VkPresentResize(VkContext* ctx, u32 width, u32 height);
void VkPresentShutdown(VkContext* ctx);
bool VkPresentIsReady();

// Uploads fb_w x fb_h BGRA8 pixels and presents them (scaled to the window).
bool VkPresentFrame(VkContext* ctx, const void* bgra_pixels, u32 fb_w, u32 fb_h);

// Clears the next swapchain image to (r,g,b,a) and presents it.
bool VkPresentClearColor(VkContext* ctx, float r, float g, float b, float a);

// M3.2d: blits a guest render-target VkImage (currently in
// COLOR_ATTACHMENT_OPTIMAL) into the swapchain image and presents it,
// restoring the source layout afterwards.  Draws and presents share the
// graphics queue, so the blit is ordered after any in-flight draws without
// extra synchronization.  `src_format` is the render target's VkFormat;
// linear-float sources are sRGB-encoded on the way to a UNORM swapchain.
bool VkPresentFromImage(VkContext* ctx, VkImage src, VkFormat src_format,
                        u32 src_w, u32 src_h);

// SharpEmu VulkanVideoPresenter.GetSrgbCounterpart / IsLinearFloatPresentSource
// (#448).  Exposed for the present-format unit tests.
//
// Maps a UNORM swapchain format to the sRGB view of the same bit layout, or
// VK_FORMAT_UNDEFINED when no counterpart exists.  Used to encode
// linear-float guest flips on their way into a UNORM swapchain.
VkFormat VkPresentSrgbCounterpart(VkFormat format);
// Float VideoOut flip buffers hold linear scRGB light; presenting them
// requires a linear->sRGB encode that a plain blit does not perform.
bool VkPresentIsLinearFloatSource(VkFormat format);

// R1: VRR / vsync configuration.  Called when config is loaded.
// `vsync` enables VK_PRESENT_MODE_FIFO_KHR; `vrr` tries FIFO_RELAXED or
// IMMEDIATE which let a VRR display (FreeSync / G-SYNC) handle tearing.
void VkPresentSetVrrConfig(bool vsync, bool vrr);

// Phase 5 validation (golden-image tests): optional per-frame readback hook.
// When installed, every presented frame is delivered to the callback as
// tightly packed BGRA8 at the guest (unscaled) resolution, read back from
// the GPU source image through a host-visible buffer (VkPresentClearColor
// synthesizes its solid frame CPU-side).  `frame_index` counts delivered
// frames from 0.  Default null hook = zero cost.  tools/pm4_replay uses this
// to dump one PNG per flip and compare against golden images.
typedef void (*VkPresentReadbackFn)(const u8* bgra, u32 w, u32 h,
                                    u64 frame_index, void* user);
void VkPresentSetReadbackHook(VkPresentReadbackFn fn, void* user);

} // namespace GPU
