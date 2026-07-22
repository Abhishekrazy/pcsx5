// Phase 5 M3.2c — Gfx10 guest render-state decoder tests (pure logic; no
// Vulkan device): VGT topology, CB blend, PA_SU_SC raster, viewport/scissor,
// CB_COLOR0 render-target decode, image/sampler descriptor decode.
// H5: full texture format table, swizzle detiling, BC format plans.

#include "gpu/gfx10_state.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;
int g_checks   = 0;

#define EXPECT(cond, msg)                                                            \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg);    \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

#define EXPECT_EQ(a, b, msg)                                                        \
    do {                                                                            \
        ++g_checks;                                                                 \
        auto _lhs = (a);                                                            \
        auto _rhs = (b);                                                            \
        if (!(_lhs == _rhs)) {                                                      \
            std::fprintf(stderr, "[FAIL] %s:%d: %s  (lhs=%lld rhs=%lld)\n",         \
                         __FILE__, __LINE__, msg,                                   \
                         (long long)_lhs, (long long)_rhs);                         \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

using namespace GPU::Gfx10;

u32 F2Bits(float f) { u32 v; std::memcpy(&v, &f, 4); return v; }

void TestTopology() {
    bool restart = false;
    EXPECT_EQ(PrimitiveTopologyFromVgt(1, &restart), VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
              "vgt 1 -> point list");
    EXPECT(!restart, "point list: no restart");
    EXPECT_EQ(PrimitiveTopologyFromVgt(4, &restart), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
              "vgt 4 -> triangle list");
    EXPECT(!restart, "triangle list: no restart");
    EXPECT_EQ(PrimitiveTopologyFromVgt(6, &restart), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
              "vgt 6 -> triangle strip");
    EXPECT(restart, "triangle strip: restart");
    EXPECT_EQ(PrimitiveTopologyFromVgt(0x11, &restart), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
              "rect list -> triangle strip");
    EXPECT(restart, "rect list: restart");
    EXPECT_EQ(PrimitiveTopologyFromVgt(999, &restart), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
              "unknown vgt -> triangle list");
}

void TestBlend() {
    // SharpEmu DecodeBlendState: bit30 enable, color src [4:0], op [7:5],
    // dst [12:8], alpha [20:16]/[23:21]/[28:24], separateAlpha bit29.
    const u32 ctl = (1u << 30) | (1u << 29) |
                    4u /* src alpha */ | (0u << 5) | 5u << 8 /* 1-src alpha */ |
                    1u << 16 /* one */ | (0u << 21) | 0u << 24;
    const BlendState b = DecodeBlendState(ctl, 0xF);
    EXPECT(b.enable, "blend enable bit30");
    EXPECT_EQ(b.src_color, VK_BLEND_FACTOR_SRC_ALPHA, "color src factor");
    EXPECT_EQ(b.dst_color, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, "color dst factor");
    EXPECT_EQ(b.color_op, VK_BLEND_OP_ADD, "color op add");
    EXPECT_EQ(b.src_alpha, VK_BLEND_FACTOR_ONE, "separate alpha src");
    EXPECT_EQ(b.write_mask, 0xFu, "write mask from target mask");

    // separateAlpha clear: alpha channel reuses the color factors.
    const BlendState c = DecodeBlendState(ctl & ~(1u << 29), 0xF);
    EXPECT_EQ(c.src_alpha, VK_BLEND_FACTOR_SRC_ALPHA, "shared alpha src factor");
    EXPECT_EQ(c.dst_alpha, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, "shared alpha dst factor");

    const BlendState off = DecodeBlendState(0, 0x3);
    EXPECT(!off.enable, "blend disabled without bit30");
    EXPECT_EQ(off.write_mask, 0x3u, "write mask nibble");
}

void TestRaster() {
    const RasterState none = DecodeRasterState(0);
    EXPECT_EQ(none.cull_mode, 0u, "no cull");
    EXPECT_EQ(none.front_face, VK_FRONT_FACE_COUNTER_CLOCKWISE, "ccw default");

    const RasterState back_cw = DecodeRasterState(2 | 4);
    EXPECT_EQ(back_cw.cull_mode, static_cast<u32>(VK_CULL_MODE_BACK_BIT), "cull back bit1");
    EXPECT_EQ(back_cw.front_face, VK_FRONT_FACE_CLOCKWISE, "cw bit2");

    const RasterState both = DecodeRasterState(1 | 2);
    EXPECT_EQ(both.cull_mode, static_cast<u32>(VK_CULL_MODE_FRONT_AND_BACK), "cull both");
}

void TestRenderTarget() {
    // 256-byte-unit address; dims from attrib2 +1; format info[6:2], num
    // info[10:8].  SharpEmu GetRenderTargets.
    RenderTargetDesc rt;
    EXPECT(DecodeRenderTarget(0x1234500u, 0, 10u << 2, 0, rt),
           "rt decode succeeds with nonzero base");
    EXPECT_EQ(rt.address, 0x1234500ull << 8, "rt address is base<<8");
    EXPECT_EQ(rt.format, 10u, "rt format field");
    EXPECT_EQ(rt.width, 1u, "rt width attrib2+1");

    const u32 attrib2 = (1919u) | ((1079u) << 14);
    EXPECT(DecodeRenderTarget(0x1000, 0x12, (10u << 2) | (1u << 8), attrib2, rt),
           "rt 1080p decode");
    EXPECT_EQ(rt.address, (0x12ull << 40) | (0x1000ull << 8), "rt hi/lo address");
    EXPECT_EQ(rt.width, 1920u, "rt width 1920");
    EXPECT_EQ(rt.height, 1080u, "rt height 1080");
    EXPECT_EQ(rt.number_type, 1u, "rt number type");

    EXPECT(!DecodeRenderTarget(0, 0, 0, attrib2, rt), "null rt rejected");

    EXPECT_EQ(RenderTargetFormat(10, 0), VK_FORMAT_R8G8B8A8_UNORM, "rt fmt 10/0");
    EXPECT_EQ(RenderTargetFormat(10, 9), VK_FORMAT_R8G8B8A8_SRGB, "rt fmt 10/9");
    EXPECT_EQ(RenderTargetFormat(12, 7), VK_FORMAT_R16G16B16A16_SFLOAT, "rt fmt 12/7");
}

void TestImageDescriptor() {
    // 256-bit 2D descriptor, linear tile mode 0, RGBA8 (unified 56), pitch.
    u32 w[8] = {};
    const u64 addr = 0x123456789000ull;
    w[0] = static_cast<u32>((addr >> 8) & 0xFFFFFFFFull);
    w[1] = static_cast<u32>((addr >> 40) & 0xFFull) |
           (56u << 20) | ((639u & 0x3u) << 30);  // width-1 low 2 bits [31:30]
    w[2] = (639u >> 2) | ((359u) << 14);         // width-1 [15:2] + height-1
    w[3] = (9u << 28) | 0xFACu;               // type 2D, identity swizzle
    w[4] = 640u - 1u;                          // pitch-1
    ImageDesc d;
    EXPECT(DecodeImageDescriptor(w, d), "image descriptor decodes");
    EXPECT_EQ(d.address, addr, "image address");
    EXPECT_EQ(d.width, 640u, "image width");
    EXPECT_EQ(d.height, 360u, "image height");
    EXPECT_EQ(d.pitch, 640u, "image pitch from w4");
    EXPECT_EQ(d.unified_format, 56u, "image unified format");
    EXPECT_EQ(d.dst_select, 0xFACu, "image identity swizzle");
    EXPECT_EQ(d.tile_mode, 0u, "image linear tile mode");

    w[4] = 0;
    EXPECT(DecodeImageDescriptor(w, d), "128-bit-style pitch decode");
    EXPECT_EQ(d.pitch, d.width, "pitch falls back to width");

    w[0] = 0; w[1] &= ~0xFFu;
    EXPECT(!DecodeImageDescriptor(w, d), "null image rejected");

    EXPECT_EQ(TextureFormat(10, 0), VK_FORMAT_R8G8B8A8_UNORM, "tex fmt 10/0");
    EXPECT_EQ(TextureFormat(10, 9), VK_FORMAT_R8G8B8A8_SRGB, "tex fmt 10/9");
    EXPECT_EQ(TextureFormat(3, 0), VK_FORMAT_R8G8_UNORM, "tex fmt 3/0");
    EXPECT_EQ(FormatBytesPerPixel(VK_FORMAT_R8G8B8A8_UNORM), 4u, "rgba8 bpp");
}

// Array/3D descriptors carry the layer count in word4[12:0] (SharpEmu
// TextureDescriptor Depth, #471); plain 2D resources are single-layer.
void TestImageDescriptorDepth() {
    u32 w[8] = {};
    const u64 addr = 0x123456789000ull;
    w[0] = static_cast<u32>((addr >> 8) & 0xFFFFFFFFull);
    w[1] = static_cast<u32>((addr >> 40) & 0xFFull) |
           (56u << 20) | ((255u & 0x3u) << 30);
    w[2] = (255u >> 2) | ((255u) << 14); // 256x256
    w[3] = (13u << 28) | 0xFACu;         // type 2D array
    w[4] = 5u;                           // DEPTH/LAST_SLICE 5 -> 6 layers
    ImageDesc d;
    EXPECT(DecodeImageDescriptor(w, d), "2D-array descriptor decodes");
    EXPECT_EQ(d.type, 13u, "2D-array type");
    EXPECT_EQ(d.depth, 6u, "array layer count from w4");

    w[3] = (9u << 28) | 0xFACu; // plain 2D
    w[4] = 256u - 1u;           // pitch field, not a layer count
    EXPECT(DecodeImageDescriptor(w, d), "2D descriptor decodes");
    EXPECT_EQ(d.depth, 1u, "plain 2D is single-layer");
}

void TestSamplerAndSwizzle() {
    // Filters: 1/3 -> linear, else nearest; address modes from w0.
    u32 w[4] = {};
    w[0] = 0u | (1u << 3) | (2u << 6); // repeat / mirrored / clamp
    w[2] = (1u << 20) | (3u << 22) | (2u << 26); // linear / linear / linear mip
    w[0] |= (0x08u << 16); // LOD bias +1.0f (8 / 8)
    w[1] = (16u) | (160u << 12); // min_lod 1.0f (16/16), max_lod 10.0f (160/16)
    w[2] |= (1u << 16) | (3u << 14); // anisotropy enable=1, level=3 (8.0x)
    const SamplerState s = DecodeSampler(w);
    EXPECT_EQ(s.mag, VK_FILTER_LINEAR, "mag linear");
    EXPECT_EQ(s.min, VK_FILTER_LINEAR, "min linear");
    EXPECT_EQ(s.mip, VK_SAMPLER_MIPMAP_MODE_LINEAR, "mip linear");
    EXPECT_EQ(s.addr_x, VK_SAMPLER_ADDRESS_MODE_REPEAT, "wrap x repeat");
    EXPECT_EQ(s.addr_y, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, "wrap y mirror");
    EXPECT_EQ(s.addr_z, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, "wrap z clamp");
    EXPECT_EQ(s.min_lod, 1.0f, "min_lod 1.0");
    EXPECT_EQ(s.max_lod, 10.0f, "max_lod 10.0");
    EXPECT_EQ(s.lod_bias, 1.0f, "lod_bias 1.0");
    EXPECT(s.anisotropy_enable, "anisotropy enable");
    EXPECT_EQ(s.max_anisotropy, 8.0f, "max_anisotropy 8.0");

    const VkComponentMapping id = DecodeComponentMapping(0xFAC);
    EXPECT_EQ(id.r, VK_COMPONENT_SWIZZLE_R, "swizzle r");
    EXPECT_EQ(id.g, VK_COMPONENT_SWIZZLE_G, "swizzle g");
    EXPECT_EQ(id.b, VK_COMPONENT_SWIZZLE_B, "swizzle b");
    EXPECT_EQ(id.a, VK_COMPONENT_SWIZZLE_A, "swizzle a");

    const VkComponentMapping bgra = DecodeComponentMapping(
        6u | (5u << 3) | (4u << 6) | (7u << 9)); // B,G,R,A
    EXPECT_EQ(bgra.r, VK_COMPONENT_SWIZZLE_B, "bgra swizzle r<-b");
    EXPECT_EQ(bgra.b, VK_COMPONENT_SWIZZLE_R, "bgra swizzle b<-r");
}

void TestViewportScissor() {
    // 1920x1080 target, standard full-frame viewport scales.
    const ViewportScissor full = DecodeViewportScissor(
        F2Bits(960.0f), F2Bits(960.0f), F2Bits(-540.0f), F2Bits(540.0f),
        0, 0, 0, 0, 0, 0, 1920, 1080);
    EXPECT_EQ(full.viewport.width, 1920.0f, "viewport width 2*xscale");
    EXPECT_EQ(full.viewport.height, -1080.0f, "negative height y-flip passthrough");
    EXPECT_EQ(full.viewport.x, 0.0f, "viewport x");
    EXPECT_EQ(full.viewport.y, 1080.0f, "viewport y = yoffset - yscale");
    EXPECT_EQ(full.scissor.extent.width, 1920u, "zero scissor pair -> full target");

    // Unusable scales fall back to the full target.
    const ViewportScissor fb = DecodeViewportScissor(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1280, 720);
    EXPECT_EQ(fb.viewport.width, 1280.0f, "fallback viewport width");
    EXPECT_EQ(fb.viewport.height, -720.0f, "fallback viewport keeps guest Y-down clip");
    EXPECT_EQ(fb.viewport.y, 720.0f, "fallback viewport y = target height");

    // Screen scissor: x[14:0], y[30:16] TL/BR dwords.
    const u32 tl = 100u | (50u << 16);
    const u32 br = 500u | (400u << 16);
    const ViewportScissor sc = DecodeViewportScissor(
        F2Bits(960.0f), F2Bits(960.0f), F2Bits(540.0f), F2Bits(540.0f),
        tl, br, 0, 0, 0, 0, 1920, 1080);
    EXPECT_EQ(sc.scissor.offset.x, 100, "scissor x");
    EXPECT_EQ(sc.scissor.offset.y, 50, "scissor y");
    EXPECT_EQ(sc.scissor.extent.width, 400u, "scissor w");
    EXPECT_EQ(sc.scissor.extent.height, 350u, "scissor h");

    // Generic scissor enabled (bit 31 set) & viewport scissor intersection:
    // Screen: [100, 50, 500, 400]
    // Generic: [150, 20, 300, 500] (bit 31 set) -> intersection [150, 50, 300, 400]
    // Vport: [50, 80, 400, 200] (bit 31 set) -> final intersection [150, 80, 300, 200]
    const u32 gen_tl = 150u | (20u << 16) | 0x80000000u;
    const u32 gen_br = 300u | (500u << 16);
    const u32 vp_tl  = 50u  | (80u << 16) | 0x80000000u;
    const u32 vp_br  = 400u | (200u << 16);
    const ViewportScissor intersected = DecodeViewportScissor(
        F2Bits(960.0f), F2Bits(960.0f), F2Bits(540.0f), F2Bits(540.0f),
        tl, br, gen_tl, gen_br, vp_tl, vp_br, 1920, 1080);
    EXPECT_EQ(intersected.scissor.offset.x, 150, "intersected scissor x");
    EXPECT_EQ(intersected.scissor.offset.y, 80, "intersected scissor y");
    EXPECT_EQ(intersected.scissor.extent.width, 150u, "intersected scissor w (300-150)");
    EXPECT_EQ(intersected.scissor.extent.height, 120u, "intersected scissor h (200-80)");

    // Generic scissor disabled (bit 31 clear) -> ignored.
    const u32 gen_disabled_tl = 150u | (20u << 16); // bit 31 clear
    const ViewportScissor gen_off = DecodeViewportScissor(
        F2Bits(960.0f), F2Bits(960.0f), F2Bits(540.0f), F2Bits(540.0f),
        tl, br, gen_disabled_tl, gen_br, 0, 0, 1920, 1080);
    EXPECT_EQ(gen_off.scissor.offset.x, 100, "disabled generic scissor ignored x");
    EXPECT_EQ(gen_off.scissor.extent.width, 400u, "disabled generic scissor ignored w");
}

// --- Phase 5 H5: full texture format table, swizzle detiling, BC plans.

void TestTextureFormatTable() {
    // Every (data_format, number_format) pair the unified-format decoder
    // (gcn_eval, RDNA2 ISA table 47) can emit, plus the BC passthrough ids
    // 169-182.  VK_FORMAT_UNDEFINED = explicitly unsupported.
    struct Row { u32 d, n; VkFormat f; };
    static const Row kRows[] = {
        {0, 0, VK_FORMAT_UNDEFINED}, // unified 0 = invalid
        {1, 0, VK_FORMAT_R8_UNORM}, {1, 1, VK_FORMAT_R8_SNORM},
        {1, 2, VK_FORMAT_R8_USCALED}, {1, 3, VK_FORMAT_R8_SSCALED},
        {1, 4, VK_FORMAT_R8_UINT}, {1, 5, VK_FORMAT_R8_SINT},
        {1, 9, VK_FORMAT_R8_SRGB},
        {2, 0, VK_FORMAT_R16_UNORM}, {2, 1, VK_FORMAT_R16_SNORM},
        {2, 2, VK_FORMAT_R16_USCALED}, {2, 3, VK_FORMAT_R16_SSCALED},
        {2, 4, VK_FORMAT_R16_UINT}, {2, 5, VK_FORMAT_R16_SINT},
        {2, 7, VK_FORMAT_R16_SFLOAT},
        {3, 0, VK_FORMAT_R8G8_UNORM}, {3, 1, VK_FORMAT_R8G8_SNORM},
        {3, 2, VK_FORMAT_R8G8_USCALED}, {3, 3, VK_FORMAT_R8G8_SSCALED},
        {3, 4, VK_FORMAT_R8G8_UINT}, {3, 5, VK_FORMAT_R8G8_SINT},
        {3, 9, VK_FORMAT_R8G8_SRGB},
        {4, 4, VK_FORMAT_R32_UINT}, {4, 5, VK_FORMAT_R32_SINT},
        {4, 7, VK_FORMAT_R32_SFLOAT},
        {5, 0, VK_FORMAT_R16G16_UNORM}, {5, 1, VK_FORMAT_R16G16_SNORM},
        {5, 2, VK_FORMAT_R16G16_USCALED}, {5, 3, VK_FORMAT_R16G16_SSCALED},
        {5, 4, VK_FORMAT_R16G16_UINT}, {5, 5, VK_FORMAT_R16G16_SINT},
        {5, 7, VK_FORMAT_R16G16_SFLOAT},
        {6, 7, VK_FORMAT_B10G11R11_UFLOAT_PACK32},
        {7, 7, VK_FORMAT_B10G11R11_UFLOAT_PACK32},
        {8, 0, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
        {8, 1, VK_FORMAT_A2B10G10R10_SNORM_PACK32},
        {8, 4, VK_FORMAT_A2B10G10R10_UINT_PACK32},
        {8, 5, VK_FORMAT_A2B10G10R10_SINT_PACK32},
        {9, 0, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
        {9, 1, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
        {9, 2, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
        {9, 3, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
        {9, 4, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
        {9, 5, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
        {10, 0, VK_FORMAT_R8G8B8A8_UNORM}, {10, 1, VK_FORMAT_R8G8B8A8_SNORM},
        {10, 2, VK_FORMAT_R8G8B8A8_USCALED}, {10, 3, VK_FORMAT_R8G8B8A8_SSCALED},
        {10, 4, VK_FORMAT_R8G8B8A8_UINT}, {10, 5, VK_FORMAT_R8G8B8A8_SINT},
        {10, 9, VK_FORMAT_R8G8B8A8_SRGB},
        {11, 4, VK_FORMAT_R32G32_UINT}, {11, 5, VK_FORMAT_R32G32_SINT},
        {11, 7, VK_FORMAT_R32G32_SFLOAT},
        {12, 0, VK_FORMAT_R16G16B16A16_UNORM},
        {12, 1, VK_FORMAT_R16G16B16A16_SNORM},
        {12, 2, VK_FORMAT_R16G16B16A16_USCALED},
        {12, 3, VK_FORMAT_R16G16B16A16_SSCALED},
        {12, 4, VK_FORMAT_R16G16B16A16_UINT},
        {12, 5, VK_FORMAT_R16G16B16A16_SINT},
        {12, 7, VK_FORMAT_R16G16B16A16_SFLOAT},
        {13, 4, VK_FORMAT_R32G32B32A32_UINT},
        {13, 5, VK_FORMAT_R32G32B32A32_SINT},
        {13, 7, VK_FORMAT_R32G32B32A32_SFLOAT},
        {14, 4, VK_FORMAT_R32G32B32A32_UINT},
        {14, 5, VK_FORMAT_R32G32B32A32_SINT},
        {14, 7, VK_FORMAT_R32G32B32A32_SFLOAT},
        {16, 0, VK_FORMAT_B5G6R5_UNORM_PACK16},
        {17, 0, VK_FORMAT_R5G5B5A1_UNORM_PACK16},
        {18, 0, VK_FORMAT_UNDEFINED}, // 5_5_5_1: no Vulkan equivalent
        {19, 0, VK_FORMAT_R4G4B4A4_UNORM_PACK16},
        {20, 0, VK_FORMAT_UNDEFINED}, {20, 4, VK_FORMAT_UNDEFINED}, // 8_24
        {21, 0, VK_FORMAT_UNDEFINED}, {21, 4, VK_FORMAT_UNDEFINED}, // 24_8
        {22, 4, VK_FORMAT_UNDEFINED}, {22, 7, VK_FORMAT_UNDEFINED}, // X24_8_32
        {32, 0, VK_FORMAT_UNDEFINED}, {32, 1, VK_FORMAT_UNDEFINED},
        {32, 4, VK_FORMAT_UNDEFINED}, {32, 9, VK_FORMAT_UNDEFINED}, // GB_GR
        {33, 0, VK_FORMAT_UNDEFINED}, {33, 1, VK_FORMAT_UNDEFINED},
        {33, 4, VK_FORMAT_UNDEFINED}, {33, 9, VK_FORMAT_UNDEFINED}, // BG_RG
        {34, 7, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32},
        {131, 7, VK_FORMAT_UNDEFINED},
        {137, 0, VK_FORMAT_UNDEFINED},
        {138, 0, VK_FORMAT_UNDEFINED},
        {139, 0, VK_FORMAT_UNDEFINED},
        // BC1-BC7: native Vulkan BC formats (no CPU decode).
        {169, 0, VK_FORMAT_BC1_RGBA_UNORM_BLOCK},
        {170, 9, VK_FORMAT_BC1_RGBA_SRGB_BLOCK},
        {171, 0, VK_FORMAT_BC2_UNORM_BLOCK},
        {172, 9, VK_FORMAT_BC2_SRGB_BLOCK},
        {173, 0, VK_FORMAT_BC3_UNORM_BLOCK},
        {174, 9, VK_FORMAT_BC3_SRGB_BLOCK},
        {175, 0, VK_FORMAT_BC4_UNORM_BLOCK},
        {176, 1, VK_FORMAT_BC4_SNORM_BLOCK},
        {177, 0, VK_FORMAT_BC5_UNORM_BLOCK},
        {178, 1, VK_FORMAT_BC5_SNORM_BLOCK},
        {179, 7, VK_FORMAT_BC6H_UFLOAT_BLOCK},
        {180, 7, VK_FORMAT_BC6H_SFLOAT_BLOCK},
        {181, 0, VK_FORMAT_BC7_UNORM_BLOCK},
        {182, 9, VK_FORMAT_BC7_SRGB_BLOCK},
    };
    for (const Row& r : kRows) {
        const TexturePlan p = PlanTextureFormat(r.d, r.n);
        EXPECT_EQ(p.vk_format, r.f, "format table vk_format");
        EXPECT(p.supported == (r.f != VK_FORMAT_UNDEFINED),
               "format table supported flag");
        EXPECT_EQ(TextureFormat(r.d, r.n), r.f, "TextureFormat matches plan");
        if (r.d >= 169 && r.d <= 182) {
            EXPECT(p.block_compressed, "BC plan flagged block-compressed");
            const bool half = r.d == 169 || r.d == 170 || r.d == 175 || r.d == 176;
            EXPECT_EQ(p.bytes_per_element, half ? 8u : 16u, "BC block bytes");
        } else if (p.supported) {
            EXPECT(!p.block_compressed, "uncompressed plan not flagged BC");
            EXPECT_EQ(p.bytes_per_element, FormatBytesPerPixel(r.f),
                      "uncompressed bytes per texel");
            EXPECT(p.bytes_per_element != 0, "uncompressed bpp nonzero");
        }
    }
    // BC block formats have no texel byte size.
    EXPECT_EQ(FormatBytesPerPixel(VK_FORMAT_BC1_RGBA_UNORM_BLOCK), 0u,
              "BC1 bpp is 0 (block-based)");
}

void TestDetileStandard256B() {
    // Mode 1 (256B_S), 1 byte/texel: block = 16x16 texels, standard
    // interleave with x on the low bit.  4x4 image; tiled[i] = i.
    std::vector<u8> tiled(256), linear(16, 0);
    for (size_t i = 0; i < tiled.size(); ++i) tiled[i] = static_cast<u8>(i);
    EXPECT(DetileSurface(tiled.data(), tiled.size(), linear.data(),
                         linear.size(), 1, 4, 4, 1),
           "detile 256B_S succeeds");
    static const u8 kExpect[16] = {
        0, 1, 4, 5,
        2, 3, 6, 7,
        8, 9, 12, 13,
        10, 11, 14, 15,
    };
    for (u32 i = 0; i < 16; ++i) {
        EXPECT_EQ(linear[i], kExpect[i], "256B_S texel offset");
    }
    u64 bytes = 0;
    EXPECT(TiledSurfaceByteCount(1, 4, 4, 1, bytes), "256B_S byte count");
    EXPECT_EQ(bytes, 256ull, "256B_S occupies one 256B block");
}

void TestDetileZ4K() {
    // Mode 4 (4K_Z), 1 byte/texel: block = 64x64 texels, Morton order with
    // y on the low bit.  4x4 image; tiled[i] = i.
    std::vector<u8> tiled(4096), linear(16, 0);
    for (size_t i = 0; i < tiled.size(); ++i) tiled[i] = static_cast<u8>(i);
    EXPECT(DetileSurface(tiled.data(), tiled.size(), linear.data(),
                         linear.size(), 4, 4, 4, 1),
           "detile 4K_Z succeeds");
    static const u8 kExpect[16] = {
        0, 2, 8, 10,
        1, 3, 9, 11,
        4, 6, 12, 14,
        5, 7, 13, 15,
    };
    for (u32 i = 0; i < 16; ++i) {
        EXPECT_EQ(linear[i], kExpect[i], "4K_Z texel offset");
    }
}

void TestDetileExact64KS() {
    // Mode 9 (64K_S), 4 bytes/texel (exact RB+ pattern): for x,y < 8 the
    // in-block byte offset is x0*4 + x1*8 + y0*16 + y1*32 + y2*64 + x2*128.
    std::vector<u8> tiled(65536), linear(8 * 8 * 4, 0);
    for (size_t i = 0; i < tiled.size(); ++i) tiled[i] = static_cast<u8>(i);
    EXPECT(DetileSurface(tiled.data(), tiled.size(), linear.data(),
                         linear.size(), 9, 8, 8, 4),
           "detile 64K_S succeeds");
    struct Spot { u32 x, y, byte; };
    static const Spot kSpots[] = {
        {0, 0, 0}, {1, 0, 4}, {2, 0, 8}, {0, 1, 16}, {1, 1, 20},
        {0, 2, 32}, {4, 0, 128}, {5, 3, 180}, {0, 4, 64}, {7, 7, 252},
    };
    for (const Spot& s : kSpots) {
        const size_t at = (static_cast<size_t>(s.y) * 8 + s.x) * 4;
        for (u32 b = 0; b < 4; ++b) {
            EXPECT_EQ(linear[at + b], tiled[s.byte + b], "64K_S texel bytes");
        }
    }
    u64 bytes = 0;
    EXPECT(TiledSurfaceByteCount(9, 8, 8, 4, bytes), "64K_S byte count");
    EXPECT_EQ(bytes, 65536ull, "64K_S occupies one 64K block");
}

void TestDetileBC1Blocks() {
    // Mode 9, 8 bytes/element (BC1 block): for x,y < 4 the in-block byte
    // offset is x0*8 + y0*16 + y1*32 + x1*64.  2x2 blocks (= 8x8 texels).
    std::vector<u8> tiled(65536, 0), linear(2 * 2 * 8, 0);
    for (u32 b = 0; b < 8; ++b) {
        tiled[b] = static_cast<u8>(0xA0u + b);        // block (0,0) at byte 0
        tiled[8 + b] = static_cast<u8>(0xB0u + b);    // block (1,0) at byte 8
        tiled[16 + b] = static_cast<u8>(0xC0u + b);   // block (0,1) at byte 16
        tiled[24 + b] = static_cast<u8>(0xD0u + b);   // block (1,1) at byte 24
    }
    EXPECT(DetileSurface(tiled.data(), tiled.size(), linear.data(),
                         linear.size(), 9, 2, 2, 8),
           "detile 64K_S BC1 blocks succeeds");
    static const u8 kBase[4] = {0xA0, 0xB0, 0xC0, 0xD0};
    for (u32 blk = 0; blk < 4; ++blk) {
        for (u32 b = 0; b < 8; ++b) {
            EXPECT_EQ(linear[blk * 8 + b],
                      static_cast<u8>(kBase[blk] + b), "BC1 block bytes");
        }
    }
    // SharpEmu's documented case: a 64x64 BC1 mode-9 image is 2 KiB linear
    // but occupies one whole 64 KiB swizzle block.
    u64 bytes = 0;
    EXPECT(TiledSurfaceByteCount(9, 16, 16, 8, bytes), "BC1 64x64 byte count");
    EXPECT_EQ(bytes, 65536ull, "BC1 64x64 occupies one 64K block");
}

void TestDetileUnsupportedModes() {
    EXPECT(!IsTiledSwizzleMode(0), "mode 0 is linear, not tiled");
    EXPECT(!IsTiledSwizzleMode(6), "mode 6 (4K_D) not in verified set");
    EXPECT(!IsTiledSwizzleMode(15), "mode 15 not a 2D swizzle mode");
    EXPECT(IsTiledSwizzleMode(1), "mode 1 supported");
    EXPECT(IsTiledSwizzleMode(27), "mode 27 supported");
    std::vector<u8> buf(64, 0);
    EXPECT(!DetileSurface(buf.data(), buf.size(), buf.data(), buf.size(),
                          6, 4, 4, 1),
           "unsupported mode detile fails");
    EXPECT(!DetileSurface(buf.data(), buf.size(), buf.data(), buf.size(),
                          9, 4, 4, 3),
           "non-power-of-two element size fails");
    u64 bytes = 0;
    EXPECT(!TiledSurfaceByteCount(9, 0, 8, 4, bytes), "zero width rejected");
    EXPECT(!TiledSurfaceByteCount(9, 8, 8, 3, bytes), "odd bpe rejected");
}

// --- U3 (SharpEmu 6ee445f): mip 0 lives at its GFX10 mip-chain offset.

void TestImageDescriptorMipLevels() {
    // 256-bit 2D descriptor, 640x360; word5[7:4] = MAX_MIP.
    u32 w[8] = {};
    const u64 addr = 0x123456789000ull;
    w[0] = static_cast<u32>((addr >> 8) & 0xFFFFFFFFull);
    w[1] = static_cast<u32>((addr >> 40) & 0xFFull) |
           (56u << 20) | ((639u & 0x3u) << 30);
    w[2] = (639u >> 2) | ((359u) << 14);
    w[3] = (9u << 28) | 0xFACu;
    w[4] = 640u - 1u;
    ImageDesc d;
    EXPECT(DecodeImageDescriptor(w, d), "mip descriptor decodes");
    EXPECT_EQ(d.mip_levels, 1u, "zeroed word5 -> single level");

    w[5] = 5u << 4; // MAX_MIP 5 -> 6 levels
    EXPECT(DecodeImageDescriptor(w, d), "max-mip descriptor decodes");
    EXPECT_EQ(d.mip_levels, 6u, "mip_levels = MAX_MIP+1");

    w[5] = 15u << 4; // 16 requested, 640x360 allows 10
    EXPECT(DecodeImageDescriptor(w, d), "oversized max-mip decodes");
    EXPECT_EQ(d.mip_levels, 10u, "mip_levels clamped to dimension maximum");
}

void TestBaseMipPlacement() {
    BaseMipPlacement p;

    // 4096x4096 RGBA8, mode 9 (64K_S), 13 levels: mips 1-5 sit between the
    // tail block and mip 0, so mip 0 lands at
    // 64K + (2048^2 + 1024^2 + 512^2 + 256^2 + 128^2) * 4.
    EXPECT(GetBaseMipPlacement(9, 4096, 4096, 4, 13, p),
           "64K_S 4096x4096 placement");
    EXPECT(!p.in_mip_tail, "4096x4096 mip 0 not in tail");
    EXPECT_EQ(p.byte_offset, 65536ull + 5586944ull * 4ull,
              "64K_S 4096x4096 mip-0 offset");
    // Per-slice chain span (#471): the mip-0 offset plus mip 0 itself.
    EXPECT_EQ(p.chain_slice_bytes,
              65536ull + 5586944ull * 4ull + 4096ull * 4096ull * 4ull,
              "64K_S 4096x4096 chain slice bytes");

    // 64x64 RGBA8, mode 9, 7 levels: the whole chain fits in the tail
    // block; mip 0 sits at element (64, 0) of the 128x128 block.
    EXPECT(GetBaseMipPlacement(9, 64, 64, 4, 7, p),
           "64K_S 64x64 placement");
    EXPECT(p.in_mip_tail, "64x64 mip 0 inside the tail block");
    EXPECT_EQ(p.tail_element_x, 64u, "tail mip-0 x");
    EXPECT_EQ(p.tail_element_y, 0u, "tail mip-0 y");
    EXPECT_EQ(p.chain_slice_bytes, 65536ull,
              "tail-resident chain spans one swizzle block");

    // 32x32 RGBA8, mode 5 (4K_S), 6 levels: mip 1 starts the tail, so
    // mip 0 follows it at one 4 KiB block.
    EXPECT(GetBaseMipPlacement(5, 32, 32, 4, 6, p),
           "4K_S 32x32 placement");
    EXPECT(!p.in_mip_tail, "32x32 mip 0 not in tail");
    EXPECT_EQ(p.byte_offset, 4096ull, "4K_S 32x32 mip-0 offset");
    EXPECT_EQ(p.chain_slice_bytes, 4096ull + 32ull * 32ull * 4ull,
              "4K_S 32x32 chain slice bytes");

    // 64x64 R8, mode 1 (256B_S), 7 levels: 256B blocks have no mip tail;
    // mip 0 follows the block-aligned smaller mips (1024 + 5*256 bytes).
    EXPECT(GetBaseMipPlacement(1, 64, 64, 1, 7, p),
           "256B_S 64x64 placement");
    EXPECT(!p.in_mip_tail, "256B_S mip 0 not in tail");
    EXPECT_EQ(p.byte_offset, 2304ull, "256B_S 64x64 mip-0 offset");
    EXPECT_EQ(p.chain_slice_bytes, 2304ull + 64ull * 64ull,
              "256B_S 64x64 chain slice bytes");

    // Single-level resources and unsupported modes keep the base address.
    EXPECT(!GetBaseMipPlacement(9, 64, 64, 4, 1, p),
           "single-level resource has no placement");
    EXPECT(!GetBaseMipPlacement(6, 64, 64, 4, 7, p),
           "unsupported mode has no placement");
}

void TestDetileTailMip0() {
    // Mode 9, 4 bytes/texel: block is 128x128 texels.  Detile the whole
    // block independently, then check the tail-resident 64x64 mip 0 at
    // element (64, 0) is lifted out as a sub-rectangle.
    std::vector<u8> tiled(65536), block_linear(65536);
    for (size_t i = 0; i < tiled.size(); ++i) tiled[i] = static_cast<u8>(i);
    EXPECT(DetileSurface(tiled.data(), tiled.size(), block_linear.data(),
                         block_linear.size(), 9, 128, 128, 4),
           "whole-block detile succeeds");

    std::vector<u8> linear(64 * 64 * 4, 0);
    EXPECT(DetileTailMip0(tiled.data(), tiled.size(), linear.data(),
                          linear.size(), 9, 64, 64, 4, 64, 0),
           "tail mip-0 detile succeeds");
    for (u32 y = 0; y < 64; ++y) {
        for (u32 x = 0; x < 64; ++x) {
            const size_t got = (static_cast<size_t>(y) * 64 + x) * 4;
            const size_t want =
                (static_cast<size_t>(y) * 128 + (64 + x)) * 4;
            for (u32 b = 0; b < 4; ++b) {
                EXPECT_EQ(linear[got + b], block_linear[want + b],
                          "tail mip-0 texel matches block sub-rect");
            }
        }
    }

    // A sub-rectangle that escapes the block is rejected.
    EXPECT(!DetileTailMip0(tiled.data(), tiled.size(), linear.data(),
                           linear.size(), 9, 64, 64, 4, 96, 0),
           "out-of-block tail rect rejected");
    // ... as is a source buffer smaller than one swizzle block.
    EXPECT(!DetileTailMip0(tiled.data(), 1024, linear.data(), linear.size(),
                           9, 64, 64, 4, 64, 0),
           "short tail source rejected");
}

} // namespace

int main() {
    TestTopology();
    TestBlend();
    TestRaster();
    TestRenderTarget();
    TestImageDescriptor();
    TestSamplerAndSwizzle();
    TestViewportScissor();
    TestTextureFormatTable();
    TestDetileStandard256B();
    TestDetileZ4K();
    TestDetileExact64KS();
    TestDetileBC1Blocks();
    TestDetileUnsupportedModes();
    TestImageDescriptorMipLevels();
    TestImageDescriptorDepth();
    TestBaseMipPlacement();
    TestDetileTailMip0();

    std::printf("gfx10_state_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
