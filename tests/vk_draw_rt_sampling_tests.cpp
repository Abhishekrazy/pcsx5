// Regression test for the render-target sampling rule (2026-09-07).
//
// A draw that samples a guest address the GPU has already rendered into must
// bind that render target's image. Uploading the address from guest memory
// returns an empty texture, because the pixels only ever existed in the
// Vulkan image. PPSA02929's final full-screen pass sampled such a surface and
// the screen went black after its splash.
//
// The Vulkan binding path itself needs a device and is covered by runtime
// evidence rather than by this test; what is pinned here is the decision rule.

#include "gpu/vk_draw.h"

#include <cstdio>

namespace {
int g_failures = 0;

void Expect(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}
} // namespace

int main() {
    using GPU::VkDrawShouldSampleRenderTarget;

    Expect(VkDrawShouldSampleRenderTarget(0x21ba00000ull, false, true),
           "a sampled binding naming a live render target uses its image");

    Expect(!VkDrawShouldSampleRenderTarget(0x215ed0000ull, false, false),
           "an ordinary texture still uploads from guest memory");

    Expect(!VkDrawShouldSampleRenderTarget(0, false, true),
           "a null address never samples a render target");

    // Storage bindings reach the GPU through the storage-image path and must
    // not be diverted here, or they would be bound with a sampler.
    Expect(!VkDrawShouldSampleRenderTarget(0x21ba00000ull, true, true),
           "a storage binding is left to the storage path");

    if (g_failures == 0) {
        std::fprintf(stdout, "vk_draw render-target sampling tests: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
