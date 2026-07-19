// Manual smoke test for the Vulkan present path (Phase 5 M1).
//
// NOT a ctest: requires a real GPU + window.  Initializes the GPU subsystem,
// feeds synthetic BGRA frames through GPU::RenderFrame (the same entry point
// libSceVideoOut SubmitFlip uses), and holds each frame briefly so the window
// can be screenshotted/inspected.  When the Vulkan backend is active the
// frames go staging-buffer -> texture -> vkCmdBlitImage -> swapchain;
// otherwise the GDI DIB fallback shows them.
//
// Exit code 0 if frames were presented without the process dying.

#include "gpu/gpu.h"
#include "common/types.h"

#include <cstdio>
#include <vector>
#include <thread>
#include <chrono>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    if (!GPU::Initialize()) {
        std::fprintf(stderr, "FATAL: GPU::Initialize failed\n");
        return 2;
    }

    constexpr u32 kW = 640, kH = 360;
    GPU::SetFramebufferConfig(kW, kH, 1 /* BGRA8 */);
    std::vector<u32> pixels(static_cast<size_t>(kW) * kH);

    for (int frame = 0; frame < 4; ++frame) {
        for (u32 y = 0; y < kH; ++y) {
            for (u32 x = 0; x < kW; ++x) {
                const u8 r = static_cast<u8>((x * 255) / (kW - 1));
                const u8 g = static_cast<u8>((y * 255) / (kH - 1));
                const u8 b = static_cast<u8>(frame * 60);
                pixels[static_cast<size_t>(y) * kW + x] =
                    0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
        // A moving white bar so frame-to-frame change is obvious.
        const u32 bar_x = static_cast<u32>((frame + 1) * kW / 5);
        for (u32 y = 0; y < kH; ++y) {
            for (u32 x = bar_x; x < bar_x + 16 && x < kW; ++x) {
                pixels[static_cast<size_t>(y) * kW + x] = 0xFFFFFFFFu;
            }
        }
        GPU::RenderFrame(reinterpret_cast<guest_addr_t>(pixels.data()));
        GPU::PollEvents();
        std::fprintf(stdout, "presented frame %d\n", frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
    }

    std::fprintf(stdout, "vk_present_smoke: done (window stays open 2s for inspection)\n");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    GPU::Shutdown();
    return 0;
}
