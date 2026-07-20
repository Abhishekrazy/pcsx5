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
// consumes these decoders to build pipelines/descriptors.  The sampled
// texture format table (PlanTextureFormat) covers every (data_format,
// number_format) pair the unified decoder can emit — unsupported pairs are
// reported explicitly — and DetileSurface deswizzles the verified Gfx10
// 2D swizzle modes (port of SharpEmu GnmTiling.cs).
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
    u32 type = 9;           // 8 = 1D, 9 = 2D, 10 = 3D, 11 = cube,
                            // 12 = 1D array, 13 = 2D array
    // Array layer count (word4 DEPTH/LAST_SLICE+1) for 3D/cube/array types;
    // 1 for plain 1D/2D resources.
    u32 depth = 1;
    // Resource mip count: MAX_MIP+1 from the extended descriptor (word5),
    // clamped to the dimension-derived maximum.  1 for 128-bit-style
    // descriptors (zeroed word5) and single-level resources.
    u32 mip_levels = 1;
};
bool DecodeImageDescriptor(const u32 w[8], ImageDesc& out);

// (data_format, number_format) -> sampled-image format (SharpEmu
// GetTextureFormat subset; BC/tiling-heavy formats fall back to RGBA8).
// Returns VK_FORMAT_UNDEFINED for pairs PlanTextureFormat reports as
// unsupported — callers must not create an image with the result then.
VkFormat TextureFormat(u32 data_format, u32 number_format);

// Full sampled-texture format plan (Phase 5 H5).  Covers every
// (data_format, number_format) pair the unified-format decoder
// (gcn_eval GcnTryDecodeUnifiedFormat, RDNA2 ISA table 47) can produce;
// anything without an exact Vulkan equivalent is explicitly unsupported
// (supported == false) rather than silently approximated.
struct TexturePlan {
    VkFormat vk_format = VK_FORMAT_UNDEFINED; // format for the VkImage/view
    bool     supported = false;         // false -> caller uses the fallback
    bool     block_compressed = false;  // 4x4-block BC; element = block
    u32      bytes_per_element = 0;     // texel bytes, or BC block bytes
};
TexturePlan PlanTextureFormat(u32 data_format, u32 number_format);

// Texel size for the uncompressed formats TextureFormat/RenderTargetFormat
// can return (0 when unknown or block-compressed — caller must not upload).
u32 FormatBytesPerPixel(VkFormat format);

// --- Gfx10 2D swizzle detiling (Phase 5 H5; port of SharpEmu GnmTiling.cs).
//
// The image descriptor's 5-bit SWIZZLE_MODE selects a power-of-two swizzle
// block (256 B / 4 KiB / 64 KiB) whose element order follows the standard
// (S) or z-order (Z) GFX10 equations, with the Oberon RB+ 64 KiB Z_X/R_X
// modes carrying exact AddrLib XOR patterns.  Supported modes match
// SharpEmu's verified-by-default set: 1 (256B_S), 4 (4K_Z), 5 (4K_S exact),
// 8 (64K_Z), 9 (64K_S exact), 24 (64K_Z_X exact), 27 (64K_R_X exact).
// Other modes return false so the caller can keep the raw bytes with a log.
//
// "Elements" are texels for uncompressed formats and 4x4 blocks for BC
// formats (bytes_per_element = 8 or 16).

// True when the swizzle mode names a tiled layout this detiler implements.
bool IsTiledSwizzleMode(u32 swizzle_mode);

// Physical byte span of the tiled surface: GNM allocates whole swizzle
// blocks, so a small image can occupy much more than its linear size
// (a 64x64 BC1 mode-9 image is 2 KiB linear but one 64 KiB block).
bool TiledSurfaceByteCount(u32 swizzle_mode, u32 elements_wide,
                           u32 elements_high, u32 bytes_per_element,
                           u64& byte_count);

// Deswizzles `tiled` into tightly packed linear row-major order.  Returns
// false (output untouched) for unsupported modes/element sizes.
bool DetileSurface(const u8* tiled, size_t tiled_size,
                   u8* linear, size_t linear_size,
                   u32 swizzle_mode, u32 elements_wide, u32 elements_high,
                   u32 bytes_per_element);

// --- GFX10 mip-chain placement (port of SharpEmu GnmTiling
// TryGetBaseMipPlacement / TryGetBlockElementDimensions, commit 6ee445f).
//
// GFX10 stores a mip chain smallest-first: the mip tail packs into the
// first swizzle block, the remaining mips follow in decreasing size, and
// mip 0 ends up at the end of the allocation.  Reading mip 0 straight from
// the descriptor address decodes a collage of the smaller mips.

// Where mip 0 lives inside a tiled mip chain: either a byte offset from the
// descriptor base address, or — when the whole chain fits inside the tail
// block — the element coordinates of mip 0 within that block.
struct BaseMipPlacement {
    u64  byte_offset = 0;     // valid when !in_mip_tail
    bool in_mip_tail = false;
    u32  tail_element_x = 0;  // valid when in_mip_tail
    u32  tail_element_y = 0;
    // Per-slice span of the whole mip chain in bytes: array slices are
    // spaced by this stride (SharpEmu GnmTiling chainSliceBytes, #471).
    u64  chain_slice_bytes = 0;
};

// Locates mip 0 for a resource with mip_levels > 1 (AddrLib
// Gfx10Lib::ComputeSurfaceInfoMacroTiled/MicroTiled chain-offset math).
// Returns false for single-level resources and unsupported layouts; the
// caller then keeps reading from the descriptor base address.
bool GetBaseMipPlacement(u32 swizzle_mode, u32 elements_wide,
                         u32 elements_high, u32 bytes_per_element,
                         u32 mip_levels, BaseMipPlacement& out);

// Detiles mip 0 when it lives inside the first swizzle block (the mip
// tail): deswizzles the whole block and lifts out the (tail_element_x,
// tail_element_y) sub-rectangle of elements_wide x elements_high.
bool DetileTailMip0(const u8* tiled, size_t tiled_size,
                    u8* linear, size_t linear_size,
                    u32 swizzle_mode, u32 elements_wide, u32 elements_high,
                    u32 bytes_per_element,
                    u32 tail_element_x, u32 tail_element_y);

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
