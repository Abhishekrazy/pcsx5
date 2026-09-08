#pragma once
#include <pcsx5/graphics/renderer.h>
#include <memory>
namespace pcsx5::graphics {
struct vulkan_options {
    bool require_validation = false;
    std::uint32_t device_index = 0;
};
// Existing system loader/SDK only. Validation requested but absent must fail,
// not silently skip. Device index selects the enumerated physical device.
[[nodiscard]] graphics_result<std::unique_ptr<renderer>>
make_vulkan_renderer(vulkan_options options = {}) noexcept;
}
