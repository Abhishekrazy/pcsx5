#pragma once
// Private deterministic failure seam. Production uses no callback; tests fail
// before the selected native operation, never falsify a successful submission.
#include <pcsx5/graphics/vulkan_renderer.h>
#include <vulkan/vulkan.h>
namespace pcsx5::graphics::detail {
enum class failure_site { image_allocation, queue_submission };
struct vulkan_failure_api {
    void* context{}; // Borrowed until renderer destruction; externally serialized.
    VkResult (*before)(void*, failure_site) noexcept{};
    // Exactly once from the destructor after all native teardown; never from
    // close itself. Context must outlive even failed factory initialization.
    void (*after_destroy)(void*, std::uint32_t validation_errors) noexcept{};
};
[[nodiscard]] graphics_result<std::unique_ptr<renderer>>
make_vulkan_renderer_with_failures(vulkan_options, vulkan_failure_api) noexcept;
}
