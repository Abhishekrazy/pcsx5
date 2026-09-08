#pragma once
#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace pcsx5::graphics {
enum class graphics_error { invalid_extent, invalid_command, unsupported, unavailable,
    out_of_memory, device_lost, host_failure, validation_failure, invalid_state };
template<class T> using graphics_result = std::expected<T, graphics_error>;
struct extent2d { std::uint32_t width; std::uint32_t height; };
struct rgba8 {
    std::uint8_t r{}, g{}, b{}, a{};
    friend constexpr bool operator==(rgba8, rgba8) = default;
};
struct point2d { std::int32_t x; std::int32_t y; };
struct triangle { std::array<point2d, 3> vertices; rgba8 color; };
struct frame_commands { extent2d extent; rgba8 clear; std::span<const triangle> triangles; };
struct rendered_image { extent2d extent; std::vector<rgba8> pixels; };
struct graphics_capabilities { std::uint32_t max_width; std::uint32_t max_height; std::uint32_t max_triangles; };

// Reject empty/oversized extent, too many commands, out-of-target vertices,
// and zero-area triangles. Validation precedes all device work.
[[nodiscard]] graphics_result<void> validate_frame(const frame_commands&,
    graphics_capabilities) noexcept;

// Sole owner of a synchronous offscreen renderer, externally serialized.
// Origin is upper-left, pixel centers (x+.5,y+.5); no blending, culling, depth,
// multisampling or sRGB. Triangles overwrite in list order. RGB/A are UNORM8.
// Shared identical edges have single coverage; which neighbor owns an exact
// edge sample is backend-defined. Interior solid colors differ
// by at most one code value after conversion. Output is tightly packed row-major.
// Input spans stay valid until return. Success includes completed GPU readback.
// No guest shaders, native handles or presentation surface cross this boundary.
class renderer {
public:
    virtual ~renderer() = default;
    renderer(const renderer&) = delete;
    renderer& operator=(const renderer&) = delete;
    renderer(renderer&&) = delete;
    renderer& operator=(renderer&&) = delete;
    [[nodiscard]] virtual graphics_capabilities capabilities() const noexcept = 0;
    [[nodiscard]] virtual graphics_result<rendered_image> render(const frame_commands&) noexcept = 0;
    [[nodiscard]] virtual std::uint32_t validation_errors() const noexcept = 0;
    // Idempotent; no future render after close, even if close reports a host error.
    [[nodiscard]] virtual graphics_result<void> close() noexcept = 0;
protected:
    renderer() = default;
};
} // namespace pcsx5::graphics
