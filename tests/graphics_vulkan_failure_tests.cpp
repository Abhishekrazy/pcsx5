#include "vulkan_renderer_test_api.h"

#include <array>
#include <cstdint>
#include <iostream>

namespace gfx = pcsx5::graphics;
namespace detail = pcsx5::graphics::detail;

namespace {
struct failure_context {
    detail::failure_site selected;
    VkResult result;
    unsigned injected{};
    unsigned allocation_visits{};
    unsigned submission_visits{};
};
VkResult inject_once(void* opaque, detail::failure_site site) noexcept {
    auto& state = *static_cast<failure_context*>(opaque);
    if (site == detail::failure_site::image_allocation) ++state.allocation_visits;
    if (site == detail::failure_site::queue_submission) ++state.submission_visits;
    if (site == state.selected && state.injected == 0) {
        ++state.injected;
        return state.result;
    }
    return VK_SUCCESS;
}
struct destruction_context {
    unsigned notifications{};
    std::uint32_t validation_errors{};
};
void observed_destruction(void* opaque, std::uint32_t errors) noexcept {
    auto& state = *static_cast<destruction_context*>(opaque);
    ++state.notifications;
    state.validation_errors = errors;
}
}

int main() {
    unsigned failures{};
    unsigned checks{};
    const auto check = [&](bool condition, const char* label) {
        ++checks;
        if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
    };
    // Inject before the actual failing native operation, after earlier resources
    // are owned. This tests unwind and error mapping, not real GPU loss or OOM.
    for (const auto site : std::array{detail::failure_site::image_allocation,
                                      detail::failure_site::queue_submission}) {
        const bool allocation = site == detail::failure_site::image_allocation;
        failure_context context{site, allocation ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_DEVICE_LOST};
        auto created = detail::make_vulkan_renderer_with_failures({true, 0}, {&context, inject_once});
        check(created.has_value(), "validation-required renderer");
        if (!created) continue;
        auto& renderer = **created;
        constexpr gfx::rgba8 clear{0, 255, 0, 255};
        const gfx::frame_commands frame{{4, 3}, clear, {}};
        check(renderer.render({{0, 3}, clear, {}}) ==
              std::unexpected(gfx::graphics_error::invalid_extent), "invalid before allocation");
        check(context.injected == 0 && context.allocation_visits == 0 && context.submission_visits == 0,
              "invalid frame never invokes native seam");
        check(renderer.render(frame) == std::unexpected(allocation ? gfx::graphics_error::out_of_memory
                                                                  : gfx::graphics_error::device_lost),
              "normalized injected error");
        check(context.injected == 1, "failure actually injected");
        check(renderer.validation_errors() == 0, "failure unwind has no validation errors");
        const auto recovered = renderer.render(frame);
        check(recovered.has_value(), "retry succeeds after synthetic failure");
        if (recovered) {
            check(recovered->extent.width == 4 && recovered->extent.height == 3 && recovered->pixels.size() == 12,
                  "retry image dimensions");
            for (const auto pixel : recovered->pixels) check(pixel == clear, "retry pixels");
        }
        check(context.allocation_visits == 2 && context.submission_visits == (allocation ? 1u : 2u),
              "expected resource/submission paths visited");
        check(renderer.close().has_value(), "close after retry");
        check(renderer.close().has_value(), "idempotent close after retry");
        check(renderer.validation_errors() == 0, "cleanup validation remains clean");
    }
    destruction_context destruction;
    {
        auto created = detail::make_vulkan_renderer_with_failures(
            {true, 0}, {&destruction, nullptr, observed_destruction});
        check(created.has_value(), "implicit cleanup renderer");
        if (created) {
            constexpr gfx::rgba8 clear{255, 0, 255, 255};
            const auto image = (*created)->render({{2, 3}, clear, {}});
            check(image.has_value(), "real render before implicit cleanup");
            if (image) {
                check(image->pixels.size() == 6, "implicit cleanup image size");
                for (const auto pixel : image->pixels) check(pixel == clear, "implicit cleanup pixels");
            }
            check(destruction.notifications == 0, "owner alive before implicit cleanup");
            // Deliberately do not call close: the actual destructor must clean
            // Vulkan resources before issuing its private observation callback.
        }
    }
    check(destruction.notifications == 1, "destructor notification exactly once");
    check(destruction.validation_errors == 0, "implicit cleanup validation clean");
    std::cout << "graphics-failures checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
