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
// extra synchronization.
bool VkPresentFromImage(VkContext* ctx, VkImage src, u32 src_w, u32 src_h);

} // namespace GPU
