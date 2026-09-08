#include <pcsx5/graphics/renderer.h>
#include <cstddef>
#include <limits>

namespace pcsx5::graphics {
graphics_result<void> validate_frame(const frame_commands& frame, graphics_capabilities caps) noexcept {
    const auto width = frame.extent.width;
    const auto height = frame.extent.height;
    constexpr auto coordinate_limit = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
    if (width == 0 || height == 0 || width > caps.max_width || height > caps.max_height ||
        width > coordinate_limit || height > coordinate_limit ||
        static_cast<std::uint64_t>(width) * height > std::numeric_limits<std::size_t>::max() / sizeof(rgba8)) {
        return std::unexpected(graphics_error::invalid_extent);
    }
    if (frame.triangles.size() > caps.max_triangles) return std::unexpected(graphics_error::invalid_command);
    for (const auto& command : frame.triangles) {
        for (const auto vertex : command.vertices) {
            if (vertex.x < 0 || vertex.y < 0 || static_cast<std::uint32_t>(vertex.x) > width ||
                static_cast<std::uint32_t>(vertex.y) > height) {
                return std::unexpected(graphics_error::invalid_command);
            }
        }
        const auto& a = command.vertices[0];
        const auto& b = command.vertices[1];
        const auto& c = command.vertices[2];
        // Coordinates are nonnegative int32. Each product and their difference
        // fit int64, without abs(INT_MIN) or arithmetic in the narrow type.
        const auto abx = static_cast<std::int64_t>(b.x) - a.x;
        const auto aby = static_cast<std::int64_t>(b.y) - a.y;
        const auto acx = static_cast<std::int64_t>(c.x) - a.x;
        const auto acy = static_cast<std::int64_t>(c.y) - a.y;
        if (abx * acy - aby * acx == 0) return std::unexpected(graphics_error::invalid_command);
    }
    return {};
}
} // namespace pcsx5::graphics
