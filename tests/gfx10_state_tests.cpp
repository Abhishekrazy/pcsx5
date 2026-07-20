// Phase 5 M3.2c — Gfx10 guest render-state decoder tests (pure logic; no
// Vulkan device): VGT topology, CB blend, PA_SU_SC raster, viewport/scissor,
// CB_COLOR0 render-target decode, image/sampler descriptor decode.

#include "gpu/gfx10_state.h"

#include <cstdio>
#include <cstring>

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

void TestSamplerAndSwizzle() {
    // Filters: 1/3 -> linear, else nearest; address modes from w0.
    u32 w[4] = {};
    w[0] = 0u | (1u << 3) | (2u << 6); // repeat / mirrored / clamp
    w[2] = (1u << 20) | (3u << 22) | (2u << 26); // linear / linear / linear mip
    const SamplerState s = DecodeSampler(w);
    EXPECT_EQ(s.mag, VK_FILTER_LINEAR, "mag linear");
    EXPECT_EQ(s.min, VK_FILTER_LINEAR, "min linear");
    EXPECT_EQ(s.mip, VK_SAMPLER_MIPMAP_MODE_LINEAR, "mip linear");
    EXPECT_EQ(s.addr_x, VK_SAMPLER_ADDRESS_MODE_REPEAT, "wrap x repeat");
    EXPECT_EQ(s.addr_y, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, "wrap y mirror");
    EXPECT_EQ(s.addr_z, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, "wrap z clamp");

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
        0, 0, 1920, 1080);
    EXPECT_EQ(full.viewport.width, 1920.0f, "viewport width 2*xscale");
    EXPECT_EQ(full.viewport.height, -1080.0f, "negative height y-flip passthrough");
    EXPECT_EQ(full.viewport.x, 0.0f, "viewport x");
    EXPECT_EQ(full.viewport.y, 1080.0f, "viewport y = yoffset - yscale");
    EXPECT_EQ(full.scissor.extent.width, 1920u, "zero scissor pair -> full target");

    // Unusable scales fall back to the full target.
    const ViewportScissor fb = DecodeViewportScissor(0, 0, 0, 0, 0, 0, 1280, 720);
    EXPECT_EQ(fb.viewport.width, 1280.0f, "fallback viewport width");
    EXPECT_EQ(fb.viewport.height, -720.0f, "fallback viewport keeps guest Y-down clip");
    EXPECT_EQ(fb.viewport.y, 720.0f, "fallback viewport y = target height");

    // Screen scissor: x[14:0], y[30:16] TL/BR dwords.
    const u32 tl = 100u | (50u << 16);
    const u32 br = 500u | (400u << 16);
    const ViewportScissor sc = DecodeViewportScissor(
        F2Bits(960.0f), F2Bits(960.0f), F2Bits(540.0f), F2Bits(540.0f),
        tl, br, 1920, 1080);
    EXPECT_EQ(sc.scissor.offset.x, 100, "scissor x");
    EXPECT_EQ(sc.scissor.offset.y, 50, "scissor y");
    EXPECT_EQ(sc.scissor.extent.width, 400u, "scissor w");
    EXPECT_EQ(sc.scissor.extent.height, 350u, "scissor h");
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

    std::printf("gfx10_state_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
