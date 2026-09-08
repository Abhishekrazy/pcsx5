#include <pcsx5/graphics/renderer.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>

namespace gfx = pcsx5::graphics;

int main() {
    unsigned failures{};
    unsigned checks{};
    const auto check = [&](bool value) { ++checks; if (!value) ++failures; };
    constexpr gfx::graphics_capabilities caps{64, 32, 2};
    constexpr gfx::rgba8 color{17, 83, 191, 127};
    std::array<gfx::triangle, 3> triangles{{
        {{{{0, 0}, {64, 0}, {0, 32}}}, color},
        {{{{64, 32}, {0, 32}, {64, 0}}}, color},
        {{{{1, 1}, {2, 1}, {1, 2}}}, color}}};
    check(gfx::validate_frame({{64, 32}, color, {}}, caps).has_value());
    check(gfx::validate_frame({{64, 32}, color, {triangles.data(), 2}}, caps).has_value());
    check(gfx::validate_frame({{64, 32}, color, triangles}, caps) ==
          std::unexpected(gfx::graphics_error::invalid_command));
    for (const auto extent : std::array<gfx::extent2d, 4>{{{0, 1}, {1, 0}, {65, 1}, {1, 33}}}) {
        check(gfx::validate_frame({extent, color, {}}, caps) ==
              std::unexpected(gfx::graphics_error::invalid_extent));
    }
    for (const auto point : std::array<gfx::point2d, 6>{{{-1, 0}, {0, -1}, {65, 0}, {0, 33},
             {std::numeric_limits<std::int32_t>::min(), 0},
             {std::numeric_limits<std::int32_t>::max(), 0}}}) {
        auto invalid = triangles[0];
        invalid.vertices[0] = point;
        check(gfx::validate_frame({{64, 32}, color, {&invalid, 1}}, caps) ==
              std::unexpected(gfx::graphics_error::invalid_command));
    }
    for (const auto vertices : std::array<std::array<gfx::point2d, 3>, 3>{{
            {{{0, 0}, {0, 0}, {1, 1}}}, {{{0, 0}, {1, 1}, {2, 2}}},
            {{{0, 1}, {1, 1}, {2, 1}}}}}) {
        const gfx::triangle degenerate{vertices, color};
        check(gfx::validate_frame({{64, 32}, color, {&degenerate, 1}}, caps) ==
              std::unexpected(gfx::graphics_error::invalid_command));
    }
    // Both windings are accepted; culling is not part of this HAL.
    auto reversed = triangles[0];
    reversed.vertices = {{{0, 32}, {64, 0}, {0, 0}}};
    check(gfx::validate_frame({{64, 32}, color, {&reversed, 1}}, caps).has_value());
    check(gfx::validate_frame({{1, 1}, color, {}}, {1, 1, 0}).has_value());
    check(gfx::validate_frame({{1, 1}, color, {}}, {0, 1, 0}) ==
          std::unexpected(gfx::graphics_error::invalid_extent));
    constexpr auto int_max = std::numeric_limits<std::int32_t>::max();
    constexpr auto uint_max = std::numeric_limits<std::uint32_t>::max();
    const gfx::triangle large{{{{0, 0}, {int_max, 0}, {0, int_max}}}, color};
    check(gfx::validate_frame({{static_cast<std::uint32_t>(int_max),
                               static_cast<std::uint32_t>(int_max)}, color, {&large, 1}},
                             {uint_max, uint_max, 1}).has_value());
    check(gfx::validate_frame({{uint_max, 1}, color, {}}, {uint_max, uint_max, 1}) ==
          std::unexpected(gfx::graphics_error::invalid_extent));
    // Exhaustive small-grid degeneracy and both winding directions, using a
    // separately expressed determinant as the test oracle.
    for (int a = 0; a < 9; ++a) for (int b = 0; b < 9; ++b) for (int c = 0; c < 9; ++c) {
        const int ax = a % 3, ay = a / 3, bx = b % 3, by = b / 3, cx = c % 3, cy = c / 3;
        const int area = ax * (by - cy) + bx * (cy - ay) + cx * (ay - by);
        const gfx::triangle candidate{{{{ax, ay}, {bx, by}, {cx, cy}}}, color};
        const auto result = gfx::validate_frame({{2, 2}, color, {&candidate, 1}}, {2, 2, 1});
        check(area == 0 ? result == std::unexpected(gfx::graphics_error::invalid_command)
                        : result.has_value());
    }
    std::cout << "graphics-frame checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
