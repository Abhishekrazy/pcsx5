// See gfx10_state.h for the design notes and porting references.
#include "gfx10_state.h"
#include <cstring>

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
    if (out.address == 0 || out.type < 8) {
        return false;
    }
    return true;
}

VkFormat TextureFormat(u32 data_format, u32 number_type) {
    switch (data_format) {
        case 1:
            if (number_type == 0) return VK_FORMAT_R8_UNORM;
            break;
        case 3:
            if (number_type == 0) return VK_FORMAT_R8G8_UNORM;
            break;
        case 4:
            if (number_type == 7) return VK_FORMAT_R32_SFLOAT;
            if (number_type == 4) return VK_FORMAT_R32_UINT;
            if (number_type == 5) return VK_FORMAT_R32_SINT;
            break;
        case 5:
            if (number_type == 0) return VK_FORMAT_R16G16_UNORM;
            if (number_type == 7) return VK_FORMAT_R16G16_SFLOAT;
            break;
        case 10:
            if (number_type == 9) return VK_FORMAT_R8G8B8A8_SRGB;
            if (number_type == 4) return VK_FORMAT_R8G8B8A8_UINT;
            if (number_type == 5) return VK_FORMAT_R8G8B8A8_SINT;
            return VK_FORMAT_R8G8B8A8_UNORM;
        case 11:
            if (number_type == 7) return VK_FORMAT_R32G32_SFLOAT;
            break;
        case 12:
            if (number_type == 7) return VK_FORMAT_R16G16B16A16_SFLOAT;
            if (number_type == 0) return VK_FORMAT_R16G16B16A16_UNORM;
            break;
        case 13: case 14:
            if (number_type == 7) return VK_FORMAT_R32G32B32A32_SFLOAT;
            break;
        default: break;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

u32 FormatBytesPerPixel(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8_UNORM: return 1;
        case VK_FORMAT_R8G8_UNORM: return 2;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32_SFLOAT: return 4;
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
        case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
        default: return 0;
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
    out.mag = to_filter((w[2] >> 20) & 0x3u);
    out.min = to_filter((w[2] >> 22) & 0x3u);
    const u32 mip = (w[2] >> 26) & 0x3u;
    out.mip = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                       : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    return out;
}

ViewportScissor DecodeViewportScissor(u32 xs_b, u32 xo_b, u32 ys_b, u32 yo_b,
                                      u32 screen_tl, u32 screen_br,
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
        out.scissor.offset = { 0, 0 };
        out.scissor.extent = { target_w, target_h };
        return out;
    }
    s32 x1 = static_cast<s32>(screen_tl & 0x7FFFu);
    s32 y1 = static_cast<s32>((screen_tl >> 16) & 0x7FFFu);
    s32 x2 = static_cast<s32>(screen_br & 0x7FFFu);
    s32 y2 = static_cast<s32>((screen_br >> 16) & 0x7FFFu);
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > static_cast<s32>(target_w)) x2 = static_cast<s32>(target_w);
    if (y2 > static_cast<s32>(target_h)) y2 = static_cast<s32>(target_h);
    if (x2 <= x1) x2 = x1 + 1;
    if (y2 <= y1) y2 = y1 + 1;
    out.scissor.offset = { x1, y1 };
    out.scissor.extent = { static_cast<u32>(x2 - x1), static_cast<u32>(y2 - y1) };
    return out;
}

} // namespace GPU::Gfx10
