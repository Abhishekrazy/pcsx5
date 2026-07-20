// Gfx10 (RDNA2) guest render-state decoders (Phase 5 M3.2c).
//
// Pure, Vulkan-call-free translations of the fixed-function register state
// the AGC submit walker shadows into plain Vulkan enums/structs.  Porting
// reference: SharpEmu AgcExports.cs (DecodeBlendState / DecodeRasterState /
// DecodeViewport / DecodeScissor / GetRenderTargets / TryDecodeTextureDescriptor)
// and VulkanVideoPresenter.cs (GetPrimitiveTopology / ToVkBlendFactor /
// ToVkBlendOp / GetRenderTargetFormat / GetTextureFormat / sampler decode).
//
// Everything here is header-only-declarable and unit-testable; vk_draw.cpp
// consumes these decoders to build pipelines/descriptors.  Only the subset
// the 2D path needs is fully fleshed out — unknown formats degrade to
// R8G8B8A8 variants with a log line at the call site.
#pragma once
#include "../common/types.h"
#include <vulkan/vulkan.h>

namespace GPU::Gfx10 {

// VGT_PRIMITIVE_TYPE (uconfig 0x242) -> topology.  restart_out is set for
// strip/fan topologies (SharpEmu PrimitiveRestartEnable).
VkPrimitiveTopology PrimitiveTopologyFromVgt(u32 vgt_primitive_type,
                                             bool* restart_out = nullptr);

// CB_BLEND0_CONTROL (context 0x1E0) + CB_TARGET_MASK (0x8E, slot 0 nibble).
struct BlendState {
    bool           enable = false;
    VkBlendFactor  src_color = VK_BLEND_FACTOR_ONE;
    VkBlendFactor  dst_color = VK_BLEND_FACTOR_ZERO;
    VkBlendOp      color_op  = VK_BLEND_OP_ADD;
    VkBlendFactor  src_alpha = VK_BLEND_FACTOR_ONE;
    VkBlendFactor  dst_alpha = VK_BLEND_FACTOR_ZERO;
    VkBlendOp      alpha_op  = VK_BLEND_OP_ADD;
    u32            write_mask = 0xF;
};
BlendState DecodeBlendState(u32 cb_blend0_control, u32 cb_target_mask);

// PA_SU_SC_MODE_CNTL (context 0x205).  Polygon mode is always FILL on the
// 2D path (wireframe is not emulated, matching SharpEmu's presenter).
struct RasterState {
    VkCullModeFlags cull_mode  = VK_CULL_MODE_NONE;
    VkFrontFace     front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
};
RasterState DecodeRasterState(u32 pa_su_sc_mode_cntl);

// CB_COLOR0_BASE/HI (0x318/0x319), CB_COLOR0_INFO (0x31C),
// CB_COLOR0_ATTRIB2 (0x3B0) — slot 0 only (2D path).  Address is in
// 256-byte units; dims come from ATTRIB2 (mip0 extent + 1).
struct RenderTargetDesc {
    u64 address = 0;
    u32 width = 0, height = 0;
    u32 format = 0, number_type = 0;
};
bool DecodeRenderTarget(u32 base_lo, u32 base_hi, u32 info, u32 attrib2,
                        RenderTargetDesc& out);

// (format, number_type) -> attachment format (SharpEmu
// TryDecodeRenderTargetFormat subset).  Falls back to R8G8B8A8_UNORM.
VkFormat RenderTargetFormat(u32 format, u32 number_type);

// Gfx10 8-dword image resource descriptor (RDNA2 ISA table 45;
// SharpEmu TryDecodeTextureDescriptor).  128-bit descriptors report
// pitch == width (linear row pitch in texels).
struct ImageDesc {
    u64 address = 0;
    u32 width = 1, height = 1;
    u32 pitch = 1;          // texels per row (linear tiling)
    u32 unified_format = 0; // raw 9-bit FORMAT field
    u32 dst_select = 0xFAC; // identity swizzle
    u32 tile_mode = 0;
    u32 type = 9;           // 8 = 1D, 9 = 2D
};
bool DecodeImageDescriptor(const u32 w[8], ImageDesc& out);

// (data_format, number_format) -> sampled-image format (SharpEmu
// GetTextureFormat subset; BC/tiling-heavy formats fall back to RGBA8).
VkFormat TextureFormat(u32 data_format, u32 number_format);

// Texel size for the formats TextureFormat/RenderTargetFormat can return
// (0 when unknown — caller must not upload).
u32 FormatBytesPerPixel(VkFormat format);

// dst_select (3 bits per channel, R at bits [2:0]; selector 4..7 = R/G/B/A,
// 0 = zero, 1 = one) -> VkComponentMapping.
VkComponentMapping DecodeComponentMapping(u32 dst_select);

// 4-dword sampler descriptor (SharpEmu presenter sampler decode; lod/border
// handling deferred — the 2D path is clamp + bi/trilinear only).
struct SamplerState {
    VkFilter             mag = VK_FILTER_LINEAR;
    VkFilter             min = VK_FILTER_LINEAR;
    VkSamplerMipmapMode  mip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode addr_x = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSamplerAddressMode addr_y = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSamplerAddressMode addr_z = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
};
SamplerState DecodeSampler(const u32 w[4]);

// PA_CL_VPORT_XSCALE/XOFFSET/YSCALE/YOFFSET (0x10F-0x112, float bits) and
// PA_SC_SCREEN_SCISSOR_TL/BR (0x0C/0x0D; an all-zero pair means "unset").
// Viewport falls back to the full (Y-flipped) target when the scales are
// unusable; negative height is passed through (Vulkan y-flip, matching
// SharpEmu).
struct ViewportScissor {
    VkViewport viewport = {};
    VkRect2D   scissor = {};
};
ViewportScissor DecodeViewportScissor(u32 xscale_bits, u32 xoffset_bits,
                                      u32 yscale_bits, u32 yoffset_bits,
                                      u32 screen_tl, u32 screen_br,
                                      u32 target_w, u32 target_h);

} // namespace GPU::Gfx10
