// Unit tests for the present-path format helpers (SharpEmu #448 port).
//
// Mirrors upstream tests/SharpEmu.Libs.Tests/VideoOut/
// VulkanPresentEncodeFormatTests.cs: UNORM swapchain formats map to their
// same-bit-layout sRGB counterpart, everything else keeps the direct blit,
// and only linear-float flip sources request the encode.

#include "gpu/vk_present.h"

#include <cstdio>

namespace {

int g_failures = 0;

void ExpectEq(VkFormat actual, VkFormat expected, const char* what) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s: got %d, expected %d\n", what,
                     static_cast<int>(actual), static_cast<int>(expected));
        ++g_failures;
    }
}

void ExpectTrue(bool v, const char* what) {
    if (!v) {
        std::fprintf(stderr, "FAIL: %s: expected true\n", what);
        ++g_failures;
    }
}

void ExpectFalse(bool v, const char* what) {
    if (v) {
        std::fprintf(stderr, "FAIL: %s: expected false\n", what);
        ++g_failures;
    }
}

} // namespace

int main() {
    // UNORM swapchain formats have sRGB counterparts.
    ExpectEq(GPU::VkPresentSrgbCounterpart(VK_FORMAT_B8G8R8A8_UNORM),
             VK_FORMAT_B8G8R8A8_SRGB, "B8G8R8A8_UNORM counterpart");
    ExpectEq(GPU::VkPresentSrgbCounterpart(VK_FORMAT_R8G8B8A8_UNORM),
             VK_FORMAT_R8G8B8A8_SRGB, "R8G8B8A8_UNORM counterpart");

    // Other swapchain formats keep the direct blit:
    // already-sRGB (their store encodes) and formats with no same-class sRGB
    // counterpart.
    ExpectEq(GPU::VkPresentSrgbCounterpart(VK_FORMAT_B8G8R8A8_SRGB),
             VK_FORMAT_UNDEFINED, "B8G8R8A8_SRGB keeps blit");
    ExpectEq(GPU::VkPresentSrgbCounterpart(VK_FORMAT_R8G8B8A8_SRGB),
             VK_FORMAT_UNDEFINED, "R8G8B8A8_SRGB keeps blit");
    ExpectEq(GPU::VkPresentSrgbCounterpart(VK_FORMAT_A2B10G10R10_UNORM_PACK32),
             VK_FORMAT_UNDEFINED, "A2B10G10R10_UNORM keeps blit");
    ExpectEq(GPU::VkPresentSrgbCounterpart(VK_FORMAT_R16G16B16A16_SFLOAT),
             VK_FORMAT_UNDEFINED, "R16G16B16A16_SFLOAT keeps blit");
    ExpectEq(GPU::VkPresentSrgbCounterpart(VK_FORMAT_R5G6B5_UNORM_PACK16),
             VK_FORMAT_UNDEFINED, "R5G6B5_UNORM keeps blit");
    ExpectEq(GPU::VkPresentSrgbCounterpart(VK_FORMAT_UNDEFINED),
             VK_FORMAT_UNDEFINED, "UNDEFINED keeps blit");

    // Float flip sources need the linear->sRGB encode.
    ExpectTrue(GPU::VkPresentIsLinearFloatSource(VK_FORMAT_R16G16B16A16_SFLOAT),
               "R16G16B16A16_SFLOAT is linear-float");
    ExpectTrue(GPU::VkPresentIsLinearFloatSource(VK_FORMAT_R32G32B32A32_SFLOAT),
               "R32G32B32A32_SFLOAT is linear-float");

    // Non-float flip sources keep the direct blit.
    ExpectFalse(GPU::VkPresentIsLinearFloatSource(VK_FORMAT_B8G8R8A8_UNORM),
                "B8G8R8A8_UNORM not linear-float");
    ExpectFalse(GPU::VkPresentIsLinearFloatSource(VK_FORMAT_R8G8B8A8_UNORM),
                "R8G8B8A8_UNORM not linear-float");
    ExpectFalse(GPU::VkPresentIsLinearFloatSource(VK_FORMAT_B8G8R8A8_SRGB),
                "B8G8R8A8_SRGB not linear-float");
    ExpectFalse(GPU::VkPresentIsLinearFloatSource(VK_FORMAT_A2B10G10R10_UNORM_PACK32),
                "A2B10G10R10_UNORM not linear-float");
    ExpectFalse(GPU::VkPresentIsLinearFloatSource(VK_FORMAT_B10G11R11_UFLOAT_PACK32),
                "B10G11R11_UFLOAT not linear-float");
    ExpectFalse(GPU::VkPresentIsLinearFloatSource(VK_FORMAT_UNDEFINED),
                "UNDEFINED not linear-float");

    if (g_failures != 0) {
        std::fprintf(stderr, "vk_present_format_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("vk_present_format_tests: all passed\n");
    return 0;
}
