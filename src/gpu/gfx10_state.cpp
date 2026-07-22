// See gfx10_state.h for the design notes and porting references.
#include "gfx10_state.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace GPU::Gfx10 {

namespace {

float Bits(u32 v) { float f; std::memcpy(&f, &v, 4); return f; }

VkBlendFactor ToVkBlendFactor(u32 factor) {
    switch (factor) {
        case 0:  return VK_BLEND_FACTOR_ZERO;
        case 1:  return VK_BLEND_FACTOR_ONE;
        case 2:  return VK_BLEND_FACTOR_SRC_COLOR;
        case 3:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 4:  return VK_BLEND_FACTOR_SRC_ALPHA;
        case 5:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 6:  return VK_BLEND_FACTOR_DST_ALPHA;
        case 7:  return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 8:  return VK_BLEND_FACTOR_DST_COLOR;
        case 9:  return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 10: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 13: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 14: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 15: return VK_BLEND_FACTOR_SRC1_COLOR;
        case 16: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case 17: return VK_BLEND_FACTOR_SRC1_ALPHA;
        case 18: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case 19: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 20: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp ToVkBlendOp(u32 op) {
    switch (op) {
        case 1:  return VK_BLEND_OP_SUBTRACT;
        case 2:  return VK_BLEND_OP_MIN;
        case 3:  return VK_BLEND_OP_MAX;
        case 4:  return VK_BLEND_OP_REVERSE_SUBTRACT;
        default: return VK_BLEND_OP_ADD;
    }
}

VkSamplerAddressMode ToVkAddressMode(u32 clamp) {
    switch (clamp) {
        case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 2: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

VkComponentSwizzle ToVkSwizzle(u32 selector) {
    switch (selector) {
        case 0:  return VK_COMPONENT_SWIZZLE_ZERO;
        case 1:  return VK_COMPONENT_SWIZZLE_ONE;
        case 4:  return VK_COMPONENT_SWIZZLE_R;
        case 5:  return VK_COMPONENT_SWIZZLE_G;
        case 6:  return VK_COMPONENT_SWIZZLE_B;
        case 7:  return VK_COMPONENT_SWIZZLE_A;
        default: return VK_COMPONENT_SWIZZLE_IDENTITY;
    }
}

} // namespace

VkPrimitiveTopology PrimitiveTopologyFromVgt(u32 vgt, bool* restart_out) {
    VkPrimitiveTopology topology;
    bool restart = false;
    switch (vgt) {
        case 1:    topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;     break;
        case 2:    topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;      break;
        case 3:    topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;     restart = true; break;
        case 5:    topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;   restart = true; break;
        case 6:    topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; restart = true; break;
        case 0x11: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; restart = true; break; // rect list
        default:   topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  break;
    }
    if (restart_out) *restart_out = restart;
    return topology;
}

BlendState DecodeBlendState(u32 ctl, u32 target_mask) {
    BlendState out;
    out.enable     = (ctl & (1u << 30)) != 0;
    out.src_color  = ToVkBlendFactor(ctl & 0x1Fu);
    out.color_op   = ToVkBlendOp((ctl >> 5) & 0x7u);
    out.dst_color  = ToVkBlendFactor((ctl >> 8) & 0x1Fu);
    out.src_alpha  = ToVkBlendFactor((ctl >> 16) & 0x1Fu);
    out.alpha_op   = ToVkBlendOp((ctl >> 21) & 0x7u);
    out.dst_alpha  = ToVkBlendFactor((ctl >> 24) & 0x1Fu);
    if ((ctl & (1u << 29)) == 0) { // !separateAlpha: alpha reuses color
        out.src_alpha = out.src_color;
        out.alpha_op  = out.color_op;
        out.dst_alpha = out.dst_color;
    }
    out.write_mask = target_mask & 0xFu;
    return out;
}

RasterState DecodeRasterState(u32 ctl) {
    RasterState out;
    const bool cull_front = (ctl & 1u) != 0;
    const bool cull_back  = (ctl & 2u) != 0;
    if (cull_front && cull_back) out.cull_mode = VK_CULL_MODE_FRONT_AND_BACK;
    else if (cull_front)         out.cull_mode = VK_CULL_MODE_FRONT_BIT;
    else if (cull_back)          out.cull_mode = VK_CULL_MODE_BACK_BIT;
    out.front_face = (ctl & 4u) != 0 ? VK_FRONT_FACE_CLOCKWISE
                                     : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    return out;
}

bool DecodeRenderTarget(u32 base_lo, u32 base_hi, u32 info, u32 attrib2,
                        RenderTargetDesc& out) {
    out.address     = (static_cast<u64>(base_hi & 0xFFu) << 40) |
                      (static_cast<u64>(base_lo) << 8);
    out.width       = (attrib2 & 0x3FFFu) + 1;
    out.height      = ((attrib2 >> 14) & 0x3FFFu) + 1;
    out.format      = (info >> 2) & 0x1Fu;
    out.number_type = (info >> 8) & 0x7u;
    return out.address != 0;
}

VkFormat RenderTargetFormat(u32 format, u32 number_type) {
    switch (format) {
        case 4: // 32
            if (number_type == 4) return VK_FORMAT_R32_UINT;
            if (number_type == 5) return VK_FORMAT_R32_SINT;
            if (number_type == 7) return VK_FORMAT_R32_SFLOAT;
            break;
        case 5: // 16_16
            if (number_type == 7) return VK_FORMAT_R16G16_SFLOAT;
            if (number_type == 0) return VK_FORMAT_R16G16_UNORM;
            break;
        case 10: // 8_8_8_8
            if (number_type == 9) return VK_FORMAT_R8G8B8A8_SRGB;
            if (number_type == 4) return VK_FORMAT_R8G8B8A8_UINT;
            if (number_type == 5) return VK_FORMAT_R8G8B8A8_SINT;
            return VK_FORMAT_R8G8B8A8_UNORM;
        case 11: // 32_32
            if (number_type == 7) return VK_FORMAT_R32G32_SFLOAT;
            break;
        case 12: // 16_16_16_16
            if (number_type == 7) return VK_FORMAT_R16G16B16A16_SFLOAT;
            if (number_type == 0) return VK_FORMAT_R16G16B16A16_UNORM;
            break;
        case 13: case 14: // 32_32_32(_32)
            if (number_type == 7) return VK_FORMAT_R32G32B32A32_SFLOAT;
            break;
        default: break;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

bool DecodeImageDescriptor(const u32 w[8], ImageDesc& out) {
    out.address        = ((static_cast<u64>(w[1] & 0xFFu) << 32) | w[0]) << 8;
    out.width          = (((w[1] >> 30) & 0x3u) | ((w[2] & 0x3FFFu) << 2)) + 1;
    out.height         = ((w[2] >> 14) & 0xFFFFu) + 1;
    out.unified_format = (w[1] >> 20) & 0x1FFu;
    out.tile_mode      = (w[3] >> 20) & 0x1Fu;
    out.type           = (w[3] >> 28) & 0xFu;
    out.dst_select     = w[3] & 0xFFFu;
    // 256-bit descriptors carry a pitch field; 128-bit ones are width-pitched.
    out.pitch = (out.type == 8 || out.type == 9 || out.type == 14) && w[4] != 0
                    ? (w[4] & 0x3FFFu) + 1
                    : out.width;
    // Array/3D/cube resources carry the layer count in word4[12:0]
    // (DEPTH/LAST_SLICE + 1); plain 1D/2D resources are single-layer.
    out.depth = (out.type >= 10 && out.type <= 13) || out.type == 15
                    ? (w[4] & 0x1FFFu) + 1
                    : 1;
    // Resource mip count: MAX_MIP lives in word5 of the extended descriptor
    // (zeroed for 128-bit-style resources, which keeps mip_levels == 1).
    // Clamp to the dimension-derived maximum, matching SharpEmu
    // TextureDescriptor.ResourceMipLevels.
    const u32 max_mip = (w[5] >> 4) & 0xFu;
    u32 max_levels = 1;
    for (u32 d = out.width > out.height ? out.width : out.height; d > 1;
         d >>= 1) {
        ++max_levels;
    }
    out.mip_levels = max_mip + 1 < max_levels ? max_mip + 1 : max_levels;
    if (out.address == 0 || out.type < 8) {
        return false;
    }
    return true;
}

TexturePlan PlanTextureFormat(u32 data_format, u32 number_type) {
    TexturePlan p;
    // SharpEmu VulkanVideoPresenter.GetTextureFormat, extended to cover every
    // pair the unified-format decoder (RDNA2 ISA table 47) can emit.  The
    // number formats are 0 UNORM, 1 SNORM, 2 USCALED, 3 SSCALED, 4 UINT,
    // 5 SINT, 7 FLOAT, 9 SRGB.
    switch (data_format) {
        case 1: // 8
            switch (number_type) {
                case 0: p.vk_format = VK_FORMAT_R8_UNORM; break;
                case 1: p.vk_format = VK_FORMAT_R8_SNORM; break;
                case 2: p.vk_format = VK_FORMAT_R8_USCALED; break;
                case 3: p.vk_format = VK_FORMAT_R8_SSCALED; break;
                case 4: p.vk_format = VK_FORMAT_R8_UINT; break;
                case 5: p.vk_format = VK_FORMAT_R8_SINT; break;
                case 9: p.vk_format = VK_FORMAT_R8_SRGB; break;
                default: return p;
            }
            break;
        case 2: // 16
            switch (number_type) {
                case 0: p.vk_format = VK_FORMAT_R16_UNORM; break;
                case 1: p.vk_format = VK_FORMAT_R16_SNORM; break;
                case 2: p.vk_format = VK_FORMAT_R16_USCALED; break;
                case 3: p.vk_format = VK_FORMAT_R16_SSCALED; break;
                case 4: p.vk_format = VK_FORMAT_R16_UINT; break;
                case 5: p.vk_format = VK_FORMAT_R16_SINT; break;
                case 7: p.vk_format = VK_FORMAT_R16_SFLOAT; break;
                default: return p;
            }
            break;
        case 3: // 8_8
            switch (number_type) {
                case 0: p.vk_format = VK_FORMAT_R8G8_UNORM; break;
                case 1: p.vk_format = VK_FORMAT_R8G8_SNORM; break;
                case 2: p.vk_format = VK_FORMAT_R8G8_USCALED; break;
                case 3: p.vk_format = VK_FORMAT_R8G8_SSCALED; break;
                case 4: p.vk_format = VK_FORMAT_R8G8_UINT; break;
                case 5: p.vk_format = VK_FORMAT_R8G8_SINT; break;
                case 9: p.vk_format = VK_FORMAT_R8G8_SRGB; break;
                default: return p;
            }
            break;
        case 4: // 32
            switch (number_type) {
                case 4: p.vk_format = VK_FORMAT_R32_UINT; break;
                case 5: p.vk_format = VK_FORMAT_R32_SINT; break;
                case 7: p.vk_format = VK_FORMAT_R32_SFLOAT; break;
                default: return p;
            }
            break;
        case 5: // 16_16
            switch (number_type) {
                case 0: p.vk_format = VK_FORMAT_R16G16_UNORM; break;
                case 1: p.vk_format = VK_FORMAT_R16G16_SNORM; break;
                case 2: p.vk_format = VK_FORMAT_R16G16_USCALED; break;
                case 3: p.vk_format = VK_FORMAT_R16G16_SSCALED; break;
                case 4: p.vk_format = VK_FORMAT_R16G16_UINT; break;
                case 5: p.vk_format = VK_FORMAT_R16G16_SINT; break;
                case 7: p.vk_format = VK_FORMAT_R16G16_SFLOAT; break;
                default: return p;
            }
            break;
        case 6: // 10_11_11 float
        case 7: // 11_11_10 float (reversed order; SharpEmu uses the same
                // approximation — no exact Vulkan 11_11_10 format exists)
            if (number_type == 7) {
                p.vk_format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
            }
            break;
        case 8: // 10_10_10_2
            switch (number_type) {
                case 0: p.vk_format = VK_FORMAT_A2B10G10R10_UNORM_PACK32; break;
                case 1: p.vk_format = VK_FORMAT_A2B10G10R10_SNORM_PACK32; break;
                case 2: p.vk_format = VK_FORMAT_A2B10G10R10_USCALED_PACK32; break;
                case 3: p.vk_format = VK_FORMAT_A2B10G10R10_SSCALED_PACK32; break;
                case 4: p.vk_format = VK_FORMAT_A2B10G10R10_UINT_PACK32; break;
                case 5: p.vk_format = VK_FORMAT_A2B10G10R10_SINT_PACK32; break;
                default: return p;
            }
            break;
        case 9: // 2_10_10_10 — SharpEmu maps every number type to UNORM.
            p.vk_format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            break;
        case 10: // 8_8_8_8
            switch (number_type) {
                case 0: p.vk_format = VK_FORMAT_R8G8B8A8_UNORM; break;
                case 1: p.vk_format = VK_FORMAT_R8G8B8A8_SNORM; break;
                case 2: p.vk_format = VK_FORMAT_R8G8B8A8_USCALED; break;
                case 3: p.vk_format = VK_FORMAT_R8G8B8A8_SSCALED; break;
                case 4: p.vk_format = VK_FORMAT_R8G8B8A8_UINT; break;
                case 5: p.vk_format = VK_FORMAT_R8G8B8A8_SINT; break;
                case 9: p.vk_format = VK_FORMAT_R8G8B8A8_SRGB; break;
                default: return p;
            }
            break;
        case 11: // 32_32
            switch (number_type) {
                case 4: p.vk_format = VK_FORMAT_R32G32_UINT; break;
                case 5: p.vk_format = VK_FORMAT_R32G32_SINT; break;
                case 7: p.vk_format = VK_FORMAT_R32G32_SFLOAT; break;
                default: return p;
            }
            break;
        case 12: // 16_16_16_16
            switch (number_type) {
                case 0: p.vk_format = VK_FORMAT_R16G16B16A16_UNORM; break;
                case 1: p.vk_format = VK_FORMAT_R16G16B16A16_SNORM; break;
                case 2: p.vk_format = VK_FORMAT_R16G16B16A16_USCALED; break;
                case 3: p.vk_format = VK_FORMAT_R16G16B16A16_SSCALED; break;
                case 4: p.vk_format = VK_FORMAT_R16G16B16A16_UINT; break;
                case 5: p.vk_format = VK_FORMAT_R16G16B16A16_SINT; break;
                case 7: p.vk_format = VK_FORMAT_R16G16B16A16_SFLOAT; break;
                default: return p;
            }
            break;
        case 13: // 32_32_32 — no R32G32B32 sampled support guarantee;
                 // SharpEmu widens to RGBA (matching its table).
            switch (number_type) {
                case 4: p.vk_format = VK_FORMAT_R32G32B32A32_UINT; break;
                case 5: p.vk_format = VK_FORMAT_R32G32B32A32_SINT; break;
                case 7: p.vk_format = VK_FORMAT_R32G32B32A32_SFLOAT; break;
                default: return p;
            }
            break;
        case 14: // 32_32_32_32
            switch (number_type) {
                case 4: p.vk_format = VK_FORMAT_R32G32B32A32_UINT; break;
                case 5: p.vk_format = VK_FORMAT_R32G32B32A32_SINT; break;
                case 7: p.vk_format = VK_FORMAT_R32G32B32A32_SFLOAT; break;
                default: return p;
            }
            break;
        case 16: // 5_6_5
            if (number_type == 0) p.vk_format = VK_FORMAT_B5G6R5_UNORM_PACK16;
            break;
        case 17: // 1_5_5_5
            if (number_type == 0) p.vk_format = VK_FORMAT_R5G5B5A1_UNORM_PACK16;
            break;
        case 18: // 5_5_5_1 — no Vulkan equivalent; explicitly unsupported.
            break;
        case 19: // 4_4_4_4
            if (number_type == 0) p.vk_format = VK_FORMAT_R4G4B4A4_UNORM_PACK16;
            break;
        // 20/21 (8_24, 24_8) and 22 (X24_8_32) are depth-stencil layouts with
        // no sampled-color Vulkan equivalent; 32/33 (GB_GR, BG_RG) are YUV
        // video formats; 131/137/138/139 are image-only encodings without a
        // public data-format meaning.  All explicitly unsupported.
        case 34: // 5_9_9_9 shared-exponent float
            if (number_type == 7) p.vk_format = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
            break;
        // BC1-BC7 (unified-format passthrough ids 169-182).  Uploaded in the
        // native BC VkFormat (desktop GPUs support the full BC set); the
        // detiler walks them at 4x4-block granularity, so no CPU decode is
        // needed — see the H5 notes in the header.
        case 169: p.vk_format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; break;
        case 170: p.vk_format = VK_FORMAT_BC1_RGBA_SRGB_BLOCK; break;
        case 171: p.vk_format = VK_FORMAT_BC2_UNORM_BLOCK; break;
        case 172: p.vk_format = VK_FORMAT_BC2_SRGB_BLOCK; break;
        case 173: p.vk_format = VK_FORMAT_BC3_UNORM_BLOCK; break;
        case 174: p.vk_format = VK_FORMAT_BC3_SRGB_BLOCK; break;
        case 175: p.vk_format = number_type == 1 ? VK_FORMAT_BC4_SNORM_BLOCK
                                                 : VK_FORMAT_BC4_UNORM_BLOCK; break;
        case 176: p.vk_format = VK_FORMAT_BC4_SNORM_BLOCK; break;
        case 177: p.vk_format = number_type == 1 ? VK_FORMAT_BC5_SNORM_BLOCK
                                                 : VK_FORMAT_BC5_UNORM_BLOCK; break;
        case 178: p.vk_format = VK_FORMAT_BC5_SNORM_BLOCK; break;
        case 179: p.vk_format = VK_FORMAT_BC6H_UFLOAT_BLOCK; break;
        case 180: p.vk_format = VK_FORMAT_BC6H_SFLOAT_BLOCK; break;
        case 181: p.vk_format = VK_FORMAT_BC7_UNORM_BLOCK; break;
        case 182: p.vk_format = VK_FORMAT_BC7_SRGB_BLOCK; break;
        default:
            return p;
    }
    if (p.vk_format == VK_FORMAT_UNDEFINED) {
        return p;
    }
    p.supported = true;
    if (data_format >= 169 && data_format <= 182) {
        p.block_compressed = true;
        p.bytes_per_element = (data_format == 169 || data_format == 170 ||
                               data_format == 175 || data_format == 176) ? 8 : 16;
    } else {
        p.bytes_per_element = FormatBytesPerPixel(p.vk_format);
        if (p.bytes_per_element == 0) { // unreachable for the table above
            p.supported = false;
            p.vk_format = VK_FORMAT_UNDEFINED;
        }
    }
    return p;
}

VkFormat TextureFormat(u32 data_format, u32 number_type) {
    return PlanTextureFormat(data_format, number_type).vk_format;
}

u32 FormatBytesPerPixel(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_SNORM:
        case VK_FORMAT_R8_USCALED:
        case VK_FORMAT_R8_SSCALED:
        case VK_FORMAT_R8_UINT:
        case VK_FORMAT_R8_SINT:
        case VK_FORMAT_R8_SRGB: return 1;
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8_USCALED:
        case VK_FORMAT_R8G8_SSCALED:
        case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R8G8_SRGB:
        case VK_FORMAT_R16_UNORM:
        case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16_USCALED:
        case VK_FORMAT_R16_SSCALED:
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16_SFLOAT:
        case VK_FORMAT_B5G6R5_UNORM_PACK16:
        case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
        case VK_FORMAT_R4G4B4A4_UNORM_PACK16: return 2;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R8G8B8A8_USCALED:
        case VK_FORMAT_R8G8B8A8_SSCALED:
        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16_SNORM:
        case VK_FORMAT_R16G16_USCALED:
        case VK_FORMAT_R16G16_SSCALED:
        case VK_FORMAT_R16G16_UINT:
        case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_USCALED_PACK32:
        case VK_FORMAT_A2B10G10R10_SSCALED_PACK32:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        case VK_FORMAT_A2B10G10R10_SINT_PACK32: return 4;
        case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_USCALED:
        case VK_FORMAT_R16G16B16A16_SSCALED:
        case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
        case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
        default: return 0; // includes all BC block formats (use TexturePlan)
    }
}

VkComponentMapping DecodeComponentMapping(u32 dst_select) {
    VkComponentMapping m = {};
    m.r = ToVkSwizzle(dst_select & 0x7u);
    m.g = ToVkSwizzle((dst_select >> 3) & 0x7u);
    m.b = ToVkSwizzle((dst_select >> 6) & 0x7u);
    m.a = ToVkSwizzle((dst_select >> 9) & 0x7u);
    return m;
}

SamplerState DecodeSampler(const u32 w[4]) {
    SamplerState out;
    auto to_filter = [](u32 f) { return (f == 1 || f == 3) ? VK_FILTER_LINEAR
                                                            : VK_FILTER_NEAREST; };
    out.addr_x = ToVkAddressMode(w[0] & 0x7u);
    out.addr_y = ToVkAddressMode((w[0] >> 3) & 0x7u);
    out.addr_z = ToVkAddressMode((w[0] >> 6) & 0x7u);

    // H8.2: LOD bias (w[0] bits 16-23, signed 8-bit 5.3 fixed point).
    s32 bias_s = static_cast<s32>(static_cast<s8>((w[0] >> 16) & 0xFFu));
    out.lod_bias = static_cast<float>(bias_s) / 8.0f;

    // Min/max LOD (w[1] bits 0-11 / 12-23, 12-bit 8.4 fixed point).
    out.min_lod = static_cast<float>((w[1] >> 0) & 0xFFFu) / 16.0f;
    out.max_lod = static_cast<float>((w[1] >> 12) & 0xFFFu) / 16.0f;

    out.mag = to_filter((w[2] >> 20) & 0x3u);
    out.min = to_filter((w[2] >> 22) & 0x3u);
    const u32 mip = (w[2] >> 26) & 0x3u;
    out.mip = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                       : VK_SAMPLER_MIPMAP_MODE_NEAREST;

    // H8.2: anisotropy (w[2] bits 14-16: 2-bit level + bit-16 enable).
    out.anisotropy_enable = ((w[2] >> 16) & 1u) != 0;
    if (out.anisotropy_enable) {
        static const float kAniso[] = {1.0f, 2.0f, 4.0f, 8.0f, 16.0f};
        out.max_anisotropy = kAniso[((w[2] >> 14) & 0x3u) < 5 ? ((w[2] >> 14) & 0x3u) : 0];
    }
    return out;
}

// Intersects three GCN scissor sources into one VkRect2D.
// Sources with TL bit-31 clear are skipped (disabled).
static VkRect2D IntersectScissors(s32 x1, s32 y1, s32 x2, s32 y2,
                                   u32 tl_bits, u32 br_bits,
                                   u32 target_w, u32 target_h) {
    if (tl_bits & 0x80000000u) {
        // Bit 31 set = enabled.  Decode signed 15-bit coords.
        s32 gx1 = static_cast<s32>(tl_bits & 0x7FFFu);
        s32 gy1 = static_cast<s32>((tl_bits >> 16) & 0x7FFFu);
        s32 gx2 = static_cast<s32>(br_bits & 0x7FFFu);
        s32 gy2 = static_cast<s32>((br_bits >> 16) & 0x7FFFu);
        if (gx1 < 0) gx1 = 0; if (gy1 < 0) gy1 = 0;
        if (gx2 > static_cast<s32>(target_w)) gx2 = target_w;
        if (gy2 > static_cast<s32>(target_h)) gy2 = target_h;
        x1 = (std::max)(x1, gx1); y1 = (std::max)(y1, gy1);
        x2 = (std::min)(x2, gx2); y2 = (std::min)(y2, gy2);
    }
    if (x2 <= x1) x2 = x1 + 1;
    if (y2 <= y1) y2 = y1 + 1;
    VkRect2D s;
    s.offset = {x1, y1};
    s.extent = {static_cast<u32>(x2 - x1), static_cast<u32>(y2 - y1)};
    return s;
}

ViewportScissor DecodeViewportScissor(u32 xs_b, u32 xo_b, u32 ys_b, u32 yo_b,
                                      u32 screen_tl, u32 screen_br,
                                      u32 generic_tl, u32 generic_br,
                                      u32 vport_tl, u32 vport_br,
                                      u32 target_w, u32 target_h) {
    ViewportScissor out;
    const float xs = Bits(xs_b), xo = Bits(xo_b);
    const float ys = Bits(ys_b), yo = Bits(yo_b);
    if (xs > 0.0f && ys != 0.0f) {
        out.viewport.x      = xo - xs;
        out.viewport.y      = yo - ys;
        out.viewport.width  = xs * 2.0f;
        out.viewport.height = ys * 2.0f; // may be negative: Vulkan y-flip
    } else {
        // No usable viewport state (e.g. a targetless composite draw): cover
        // the full target with the same Y-down guest clip convention the
        // register path expresses through a negative yscale — otherwise the
        // draw lands upside down.
        out.viewport.x      = 0.0f;
        out.viewport.y      = static_cast<float>(target_h);
        out.viewport.width  = static_cast<float>(target_w);
        out.viewport.height = -static_cast<float>(target_h);
    }
    out.viewport.minDepth = 0.0f;
    out.viewport.maxDepth = 1.0f;

    // An all-zero screen-scissor pair is the reset placeholder: full target.
    if (screen_tl == 0 && screen_br == 0) {
        screen_tl = 0u;
        screen_br = (target_h << 16) | target_w;
    }
    // Decode screen scissor as the base rect.
    s32 x1 = static_cast<s32>(screen_tl & 0x7FFFu);
    s32 y1 = static_cast<s32>((screen_tl >> 16) & 0x7FFFu);
    s32 x2 = static_cast<s32>(screen_br & 0x7FFFu);
    s32 y2 = static_cast<s32>((screen_br >> 16) & 0x7FFFu);
    if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
    if (x2 > static_cast<s32>(target_w)) x2 = static_cast<s32>(target_w);
    if (y2 > static_cast<s32>(target_h)) y2 = static_cast<s32>(target_h);
    if (x2 <= x1) x2 = x1 + 1;
    if (y2 <= y1) y2 = y1 + 1;

    // H8.3: intersect with generic scissor (enabled when TL bit 31 set).
    out.scissor = IntersectScissors(x1, y1, x2, y2,
                                     generic_tl, generic_br,
                                     target_w, target_h);
    out.scissor = IntersectScissors(out.scissor.offset.x, out.scissor.offset.y,
                                     out.scissor.offset.x + static_cast<s32>(out.scissor.extent.width),
                                     out.scissor.offset.y + static_cast<s32>(out.scissor.extent.height),
                                     vport_tl, vport_br,
                                     target_w, target_h);
    return out;
}

// ---------------------------------------------------------------------------
// Gfx10 2D swizzle detiler (port of SharpEmu GnmTiling.cs).
// ---------------------------------------------------------------------------
namespace {

// One address bit of a swizzle pattern: an XOR of X bits and Y bits whose
// parity becomes the output bit.
struct AddressBit {
    u32 x_mask, y_mask;
};

constexpr AddressBit kZero{0, 0};
constexpr AddressBit X(int bit) { return {1u << bit, 0}; }
constexpr AddressBit Y(int bit) { return {0, 1u << bit}; }
constexpr AddressBit XY(int x_bit, int y_bit) {
    return {1u << x_bit, 1u << y_bit};
}
constexpr AddressBit XY(int x_bit, int y_bit1, int y_bit2) {
    return {1u << x_bit, (1u << y_bit1) | (1u << y_bit2)};
}

u32 PopCount(u32 v) {
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    return (((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
}

// Oberon uses the 16-pipe / 8-pixel-packer RB+ topology; these are the
// single-sample 64 KiB equations AMD AddrLib generates for it, indexed by
// log2(bytes_per_element).
constexpr AddressBit kRbPlus64KRenderX[5][16] = {
    // 1 byte/element
    {X(0), X(1), X(2), X(3), Y(0), Y(1), Y(2), Y(3),
     XY(7, 4, 7), XY(4, 4), XY(6, 5), XY(5, 6),
     X(6), Y(6), XY(7, 8), XY(8, 7)},
    // 2 bytes/element
    {kZero, X(0), X(1), X(2), Y(0), Y(1), Y(2), X(3),
     XY(7, 4, 7), XY(4, 4), XY(6, 5), XY(5, 6),
     Y(3), X(6), XY(7, 7), XY(8, 6)},
    // 4 bytes/element
    {kZero, kZero, X(0), X(1), Y(0), Y(1), X(2), Y(2),
     XY(7, 4, 7), XY(4, 4), XY(6, 5), XY(5, 6),
     X(3), Y(3), XY(6, 7), XY(7, 6)},
    // 8 bytes/element
    {kZero, kZero, kZero, X(0), Y(0), X(1), X(2), Y(1),
     XY(7, 4, 7), XY(4, 4), XY(6, 5), XY(5, 6),
     Y(2), X(3), XY(7, 3), XY(6, 6)},
    // 16 bytes/element
    {kZero, kZero, kZero, kZero, X(0), Y(0), X(1), Y(1),
     XY(7, 4, 7), XY(4, 4), XY(6, 5), XY(5, 6),
     X(2), Y(2), XY(6, 3), XY(3, 6)},
};

constexpr AddressBit kRbPlus64KDepthX[5][16] = {
    {X(0), Y(0), X(1), Y(1), X(2), Y(2), X(3), Y(3),
     XY(7, 4, 7), XY(4, 4), XY(6, 5), XY(5, 6),
     X(6), Y(6), XY(7, 8), XY(8, 7)},
    {kZero, X(0), Y(0), X(1), Y(1), X(2), Y(2), X(3),
     XY(7, 4, 7), XY(4, 4), XY(6, 5), XY(5, 6),
     Y(3), X(6), XY(7, 7), XY(8, 6)},
    {kZero, kZero, X(0), Y(0), X(1), Y(1), X(2), Y(2),
     XY(7, 4, 7), XY(4, 4), XY(6, 5), XY(5, 6),
     X(3), Y(3), XY(6, 7), XY(7, 6)},
    {kZero, kZero, kZero, X(0), Y(0), X(1), Y(1), X(2),
     XY(7, 4, 7), XY(4, 4), XY(6, 5), XY(5, 6),
     Y(2), X(3), XY(7, 3), XY(6, 6)},
    // 16 bytes/element is identical to R_X for a 2D single-sample image.
    {kZero, kZero, kZero, kZero, X(0), Y(0), X(1), Y(1),
     XY(7, 4, 7), XY(4, 4), XY(6, 5), XY(5, 6),
     X(2), Y(2), XY(6, 3), XY(3, 6)},
};

constexpr AddressBit kRbPlus64KStandard[5][16] = {
    // GFX10_SW_64K_S_RBPLUS_PATINFO, 1 byte/element.
    {X(0), X(1), X(2), X(3), Y(0), Y(1), Y(2), Y(3),
     Y(4), X(4), Y(5), X(5), Y(6), X(6), Y(7), X(7)},
    {kZero, X(0), X(1), X(2), Y(0), Y(1), Y(2), X(3),
     Y(3), X(4), Y(4), X(5), Y(5), X(6), Y(6), X(7)},
    {kZero, kZero, X(0), X(1), Y(0), Y(1), Y(2), X(2),
     Y(3), X(3), Y(4), X(4), Y(5), X(5), Y(6), X(6)},
    // 8 bytes/element (also BC1/BC4 compressed blocks).
    {kZero, kZero, kZero, X(0), Y(0), Y(1), X(1), X(2),
     Y(2), X(3), Y(3), X(4), Y(4), X(5), Y(5), X(6)},
    // 16 bytes/element (also 16-byte BC compressed blocks).
    {kZero, kZero, kZero, kZero, Y(0), Y(1), X(0), X(1),
     Y(2), X(2), Y(3), X(3), Y(4), X(4), Y(5), X(5)},
};

// GFX10 4K_S has a separate 12-bit micro-tile equation; the generic 64K
// interleave leaves a regular grid in linearized atlases.
constexpr AddressBit kStandard4K[5][12] = {
    {X(0), X(1), X(2), X(3), Y(0), Y(1), Y(2), Y(3),
     Y(4), X(4), Y(5), X(5)},
    {kZero, X(0), X(1), X(2), Y(0), Y(1), Y(2), X(3),
     Y(3), X(4), Y(4), X(5)},
    {kZero, kZero, X(0), X(1), Y(0), Y(1), Y(2), X(2),
     Y(3), X(3), Y(4), X(4)},
    {kZero, kZero, kZero, X(0), Y(0), Y(1), X(1), X(2),
     Y(2), X(3), Y(3), X(4)},
    {kZero, kZero, kZero, kZero, Y(0), Y(1), X(0), X(1),
     Y(2), X(2), Y(3), X(3)},
};

enum class SwizzleKind { Standard, ZOrder };

int BitLog2(u32 v) {
    if (v == 0 || (v & (v - 1)) != 0) return -1;
    int log = 0;
    while (v >>= 1) ++log;
    return log;
}

// GFX10 AddrLib SWIZZLE_MODE enumeration:
//   1-3   = 256 B S/D/R    4-7   = 4 KiB Z/S/D/R    8-11  = 64 KiB Z/S/D/R
//   16-19 = 64 KiB _T      20-23 = 4 KiB _X         24-27 = 64 KiB _X
// The pipe/bank XOR (_T/_X) affects which block a tile lands in; the
// within-block element order matches the base S/Z equation.
bool SwizzleKindAndBlockBytes(u32 mode, SwizzleKind& kind, u32& block_bytes) {
    kind = SwizzleKind::Standard;
    switch (mode) {
        case 4: case 8: case 16: case 20: case 24:
            kind = SwizzleKind::ZOrder;
            break;
        case 1: case 2: case 3:
        case 5: case 6: case 7:
        case 9: case 10: case 11:
        case 17: case 18: case 19:
        case 21: case 22: case 23:
        case 25: case 26: case 27:
            break;
        default:
            return false;
    }
    if (mode >= 1 && mode <= 3)       block_bytes = 256;
    else if (mode >= 4 && mode <= 7)  block_bytes = 4096;
    else if (mode >= 20 && mode <= 23) block_bytes = 4096;
    else                              block_bytes = 65536;
    return true;
}

// Square-ish power-of-two element grid for a swizzle block (x gets the
// extra bit when the element count is not a perfect square).
bool SquareBlockDimensions(u32 block_elements, u32& width, u32& height) {
    const int total_bits = BitLog2(block_elements);
    if (total_bits < 0) return false;
    const int width_bits = (total_bits + 1) / 2;
    const int height_bits = total_bits - width_bits;
    width = 1u << width_bits;
    height = 1u << height_bits;
    return true;
}

// Exact XOR patterns carried for the Oberon RB+ modes (and 4K_S); other
// modes use the generic block-interior interleave below.
const AddressBit* ExactXorPattern(u32 swizzle_mode, int bpp_log2, u32& bit_count) {
    if (bpp_log2 < 0 || bpp_log2 >= 5) return nullptr;
    switch (swizzle_mode) {
        case 5:  bit_count = 12; return kStandard4K[bpp_log2];
        case 9:  bit_count = 16; return kRbPlus64KStandard[bpp_log2];
        case 24: bit_count = 16; return kRbPlus64KDepthX[bpp_log2];
        case 27: bit_count = 16; return kRbPlus64KRenderX[bpp_log2];
        default: return nullptr;
    }
}

u32 ComputePatternOffset(u32 x, u32 y, const AddressBit* pattern, u32 bit_count) {
    u32 offset = 0;
    for (u32 bit = 0; bit < bit_count; ++bit) {
        const u32 parity = (PopCount(x & pattern[bit].x_mask) +
                            PopCount(y & pattern[bit].y_mask)) & 1u;
        offset |= parity << bit;
    }
    return offset;
}

// Standard (S) 2D swizzle: interleave x and y bits, x taking the low bit.
u32 StandardSwizzleOffset(u32 x, u32 y, int width_bits, int height_bits) {
    u32 offset = 0;
    int out_bit = 0, xi = 0, yi = 0;
    while (xi < width_bits || yi < height_bits) {
        if (xi < width_bits) offset |= ((x >> xi++) & 1u) << out_bit++;
        if (yi < height_bits) offset |= ((y >> yi++) & 1u) << out_bit++;
    }
    return offset;
}

// Z-order (Morton) swizzle: pure bit interleave, y taking the low bit.
u32 MortonInterleave(u32 x, u32 y, int width_bits, int height_bits) {
    u32 offset = 0;
    int out_bit = 0, xi = 0, yi = 0;
    while (xi < width_bits || yi < height_bits) {
        if (yi < height_bits) offset |= ((y >> yi++) & 1u) << out_bit++;
        if (xi < width_bits) offset |= ((x >> xi++) & 1u) << out_bit++;
    }
    return offset;
}

} // namespace

bool IsTiledSwizzleMode(u32 swizzle_mode) {
    // Verified-by-default set (matches SharpEmu's IsTrustedByDefault): the
    // exact base S/Z modes plus the RB+ 64 KiB Z_X/R_X equations.
    return swizzle_mode == 1 || swizzle_mode == 4 || swizzle_mode == 5 ||
           swizzle_mode == 8 || swizzle_mode == 9 || swizzle_mode == 24 ||
           swizzle_mode == 27;
}

bool TiledSurfaceByteCount(u32 swizzle_mode, u32 elements_wide,
                           u32 elements_high, u32 bytes_per_element,
                           u64& byte_count) {
    byte_count = 0;
    SwizzleKind kind;
    u32 block_bytes = 0;
    if (!IsTiledSwizzleMode(swizzle_mode) || elements_wide == 0 ||
        elements_high == 0 ||
        !SwizzleKindAndBlockBytes(swizzle_mode, kind, block_bytes)) {
        return false;
    }
    const int bpp_log2 = BitLog2(bytes_per_element);
    if (bpp_log2 < 0) return false;
    u32 block_width = 0, block_height = 0;
    if (!SquareBlockDimensions(block_bytes >> bpp_log2, block_width,
                               block_height)) {
        return false;
    }
    const u64 blocks_wide = (elements_wide + block_width - 1) / block_width;
    const u64 blocks_high = (elements_high + block_height - 1) / block_height;
    byte_count = blocks_wide * blocks_high * block_bytes;
    return true;
}

bool DetileSurface(const u8* tiled, size_t tiled_size,
                   u8* linear, size_t linear_size,
                   u32 swizzle_mode, u32 elements_wide, u32 elements_high,
                   u32 bytes_per_element) {
    if (!IsTiledSwizzleMode(swizzle_mode) || tiled == nullptr ||
        linear == nullptr || elements_wide == 0 || elements_high == 0) {
        return false;
    }
    SwizzleKind kind;
    u32 block_bytes = 0;
    if (!SwizzleKindAndBlockBytes(swizzle_mode, kind, block_bytes)) {
        return false;
    }
    const int bpp_log2 = BitLog2(bytes_per_element);
    if (bpp_log2 < 0) {
        return false; // non-power-of-two element size is not modeled
    }
    u32 block_width = 0, block_height = 0;
    if (!SquareBlockDimensions(block_bytes >> bpp_log2, block_width,
                               block_height)) {
        return false;
    }
    const u32 blocks_per_row = (elements_wide + block_width - 1) / block_width;
    const u64 required_linear =
        static_cast<u64>(elements_wide) * elements_high * bytes_per_element;
    if (linear_size < required_linear) return false;

    // Precompute the within-block element offset per (x, y) for the generic
    // interleave modes; the exact-pattern modes compute it per element.
    u32 pattern_bits = 0;
    const AddressBit* pattern = ExactXorPattern(swizzle_mode, bpp_log2,
                                                pattern_bits);
    std::vector<u32> block_table;
    if (pattern == nullptr) {
        block_table.resize(static_cast<size_t>(block_width) * block_height);
        const int width_bits = BitLog2(block_width);
        const int height_bits = BitLog2(block_height);
        for (u32 by = 0; by < block_height; ++by) {
            for (u32 bx = 0; bx < block_width; ++bx) {
                block_table[by * block_width + bx] =
                    kind == SwizzleKind::ZOrder
                        ? MortonInterleave(bx, by, width_bits, height_bits)
                        : StandardSwizzleOffset(bx, by, width_bits, height_bits);
            }
        }
    }

    const u32 block_elements = block_bytes >> bpp_log2;
    for (u32 y = 0; y < elements_high; ++y) {
        const u32 block_y = y / block_height;
        const u32 in_block_y = y % block_height;
        const u64 row_block_base = static_cast<u64>(block_y) * blocks_per_row;
        const u64 dest_row_base =
            static_cast<u64>(y) * elements_wide * bytes_per_element;
        for (u32 x = 0; x < elements_wide; ++x) {
            const u32 block_x = x / block_width;
            const u32 in_block_x = x % block_width;
            const u64 block_index = row_block_base + block_x;
            const u64 source_byte =
                pattern != nullptr
                    ? block_index * block_bytes +
                          ComputePatternOffset(x, y, pattern, pattern_bits)
                    : (block_index * block_elements +
                       block_table[in_block_y * block_width + in_block_x]) *
                          bytes_per_element;
            const u64 dest_byte = dest_row_base +
                static_cast<u64>(x) * bytes_per_element;
            if (source_byte + bytes_per_element > tiled_size ||
                dest_byte + bytes_per_element > linear_size) {
                continue;
            }
            std::memcpy(linear + dest_byte, tiled + source_byte,
                        bytes_per_element);
        }
    }
    return true;
}

// GFX10 stores a mip chain smallest-first (SharpEmu commit 6ee445f, port of
// GnmTiling.TryGetBaseMipPlacement — AddrLib
// Gfx10Lib::ComputeSurfaceInfoMacroTiled/MicroTiled chain-offset math).
bool GetBaseMipPlacement(u32 swizzle_mode, u32 elements_wide,
                         u32 elements_high, u32 bytes_per_element,
                         u32 mip_levels, BaseMipPlacement& out) {
    out = BaseMipPlacement{};
    SwizzleKind kind;
    u32 block_bytes = 0;
    if (mip_levels <= 1 || !IsTiledSwizzleMode(swizzle_mode) ||
        elements_wide == 0 || elements_high == 0 ||
        !SwizzleKindAndBlockBytes(swizzle_mode, kind, block_bytes)) {
        return false;
    }
    const int bpp_log2 = BitLog2(bytes_per_element);
    if (bpp_log2 < 0) return false;
    u32 block_width = 0, block_height = 0;
    if (!SquareBlockDimensions(block_bytes >> bpp_log2, block_width,
                               block_height)) {
        return false;
    }
    const int block_size_log2 = BitLog2(block_bytes);
    if (block_size_log2 < 8) return false;

    const u32 levels = mip_levels > 16 ? 16 : mip_levels;
    const int max_mips_in_tail = block_size_log2 <= 8 ? 0
        : block_size_log2 <= 11 ? 1 + (1 << (block_size_log2 - 9))
                                : block_size_log2 - 4;
    const u32 tail_width =
        (block_size_log2 & 1) != 0 ? block_width >> 1 : block_width;
    const u32 tail_height =
        (block_size_log2 & 1) != 0 ? block_height : block_height >> 1;

    // Walk the chain largest-first until a mip small enough for the tail
    // block (with room for every smaller mip) is found.
    u32 first_mip_in_tail = levels;
    u64 mip_sizes[16] = {};
    for (u32 i = 0; i < levels; ++i) {
        const u32 mip_width = std::max(elements_wide >> i, 1u);
        const u32 mip_height = std::max(elements_high >> i, 1u);
        if (max_mips_in_tail > 0 && mip_width <= tail_width &&
            mip_height <= tail_height &&
            levels - i <= static_cast<u32>(max_mips_in_tail)) {
            first_mip_in_tail = i;
            break;
        }
        const u64 aligned_width =
            (mip_width + block_width - 1ull) / block_width * block_width;
        const u64 aligned_height =
            (mip_height + block_height - 1ull) / block_height * block_height;
        mip_sizes[i] = aligned_width * aligned_height * bytes_per_element;
    }

    if (first_mip_in_tail == 0) {
        // The whole chain fits in the tail block: locate mip 0's element
        // coordinates inside it (AddrLib GetMipOffsetInTail deinterleave).
        const int m = max_mips_in_tail - 1;
        const u32 mip_offset =
            m > 6 ? 16u << m : static_cast<u32>(m) << 8;
        u32 mip_x = ((mip_offset >> 9) & 1u) | ((mip_offset >> 10) & 2u) |
                    ((mip_offset >> 11) & 4u) | ((mip_offset >> 12) & 8u) |
                    ((mip_offset >> 13) & 16u) | ((mip_offset >> 14) & 32u);
        u32 mip_y = ((mip_offset >> 8) & 1u) | ((mip_offset >> 9) & 2u) |
                    ((mip_offset >> 10) & 4u) | ((mip_offset >> 11) & 8u) |
                    ((mip_offset >> 12) & 16u) | ((mip_offset >> 13) & 32u);
        if ((block_size_log2 & 1) != 0) {
            std::swap(mip_x, mip_y);
            if ((bpp_log2 & 1) != 0) {
                mip_y = (mip_y << 1) | (mip_x & 1u);
                mip_x >>= 1;
            }
        }
        u32 micro_width = 0, micro_height = 0;
        if (!SquareBlockDimensions(256u >> bpp_log2, micro_width,
                                   micro_height) ||
            micro_width == 0 || micro_height == 0) {
            return false;
        }
        const u32 tail_x = mip_x * micro_width;
        const u32 tail_y = mip_y * micro_height;
        if (tail_x + elements_wide > block_width ||
            tail_y + elements_high > block_height) {
            return false;
        }
        out.in_mip_tail = true;
        out.tail_element_x = tail_x;
        out.tail_element_y = tail_y;
        // The whole chain packs into the one tail block; slices are spaced
        // by a single swizzle block.
        out.chain_slice_bytes = block_bytes;
        return true;
    }

    // Mip 0 follows the tail block and every larger mip below it; the
    // per-slice chain span additionally covers mip 0 itself.
    u64 offset = first_mip_in_tail < levels ? block_bytes : 0;
    out.chain_slice_bytes = offset;
    for (u32 i = first_mip_in_tail - 1; i >= 1; --i) {
        offset += mip_sizes[i];
    }
    for (u32 i = 0; i < first_mip_in_tail; ++i) {
        out.chain_slice_bytes += mip_sizes[i];
    }
    out.byte_offset = offset;
    return true;
}

// Tail-resident mip 0 (SharpEmu TryDetileTextureSource baseMipInTail path):
// deswizzle the whole first block, then copy the mip-0 sub-rectangle out
// of it row by row.
bool DetileTailMip0(const u8* tiled, size_t tiled_size,
                    u8* linear, size_t linear_size,
                    u32 swizzle_mode, u32 elements_wide, u32 elements_high,
                    u32 bytes_per_element,
                    u32 tail_element_x, u32 tail_element_y) {
    SwizzleKind kind;
    u32 block_bytes = 0;
    if (!IsTiledSwizzleMode(swizzle_mode) || tiled == nullptr ||
        linear == nullptr || elements_wide == 0 || elements_high == 0 ||
        !SwizzleKindAndBlockBytes(swizzle_mode, kind, block_bytes)) {
        return false;
    }
    const int bpp_log2 = BitLog2(bytes_per_element);
    if (bpp_log2 < 0) return false;
    u32 block_width = 0, block_height = 0;
    if (!SquareBlockDimensions(block_bytes >> bpp_log2, block_width,
                               block_height)) {
        return false;
    }
    if (tail_element_x + elements_wide > block_width ||
        tail_element_y + elements_high > block_height ||
        tiled_size < block_bytes ||
        linear_size < static_cast<u64>(elements_wide) * elements_high *
                          bytes_per_element) {
        return false;
    }
    std::vector<u8> block_linear(block_bytes);
    if (!DetileSurface(tiled, block_bytes, block_linear.data(),
                       block_linear.size(), swizzle_mode, block_width,
                       block_height, bytes_per_element)) {
        return false;
    }
    const size_t row_bytes =
        static_cast<size_t>(elements_wide) * bytes_per_element;
    for (u32 y = 0; y < elements_high; ++y) {
        const size_t source =
            (static_cast<size_t>(tail_element_y + y) * block_width +
             tail_element_x) * bytes_per_element;
        std::memcpy(linear + static_cast<size_t>(y) * row_bytes,
                    block_linear.data() + source, row_bytes);
    }
    return true;
}

} // namespace GPU::Gfx10
