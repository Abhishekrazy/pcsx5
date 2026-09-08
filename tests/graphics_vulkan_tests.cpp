#include <pcsx5/graphics/vulkan_renderer.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

namespace gfx = pcsx5::graphics;

namespace {
unsigned failures{};
unsigned checks{};
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
bool near(gfx::rgba8 actual, gfx::rgba8 expected) {
    const auto channel = [](std::uint8_t a, std::uint8_t b) {
        const int delta = static_cast<int>(a) - static_cast<int>(b);
        return delta >= -1 && delta <= 1;
    };
    return channel(actual.r, expected.r) && channel(actual.g, expected.g) &&
           channel(actual.b, expected.b) && channel(actual.a, expected.a);
}
template<class Oracle>
void image_is(const gfx::graphics_result<gfx::rendered_image>& result,
              gfx::extent2d extent, Oracle expected) {
    check(result.has_value(), "render succeeded");
    if (!result) return;
    check(result->extent.width == extent.width && result->extent.height == extent.height,
          "returned extent");
    check(result->pixels.size() == static_cast<std::size_t>(extent.width) * extent.height,
          "packed pixel count");
    if (result->pixels.size() != static_cast<std::size_t>(extent.width) * extent.height) return;
    for (std::uint32_t y = 0; y < extent.height; ++y)
        for (std::uint32_t x = 0; x < extent.width; ++x)
            check(near(result->pixels[static_cast<std::size_t>(y) * extent.width + x],
                       expected(x, y)), "independent pixel oracle");
}
void corpus(gfx::renderer& renderer) {
    constexpr gfx::extent2d extent{16, 12};
    constexpr gfx::rgba8 clear{19, 43, 79, 113};
    constexpr gfx::rgba8 first{211, 67, 139, 173};
    constexpr gfx::rgba8 second{31, 193, 101, 229};
    const auto caps = renderer.capabilities();
    check(caps.max_width >= 16 && caps.max_height >= 12 && caps.max_triangles >= 4,
          "synthetic corpus capabilities");
    image_is(renderer.render({extent, clear, {}}), extent,
             [clear](auto, auto) { return clear; });
    // Two opposite-winding triangles cover an integer-aligned rectangle. This
    // oracle uses rectangle membership, not the implementation's raster math.
    const std::array<gfx::triangle, 4> rectangles{{
        {{{{2, 2}, {14, 2}, {2, 10}}}, first},
        {{{{14, 10}, {14, 2}, {2, 10}}}, first},
        {{{{6, 3}, {12, 3}, {6, 7}}}, second},
        {{{{12, 7}, {12, 3}, {6, 7}}}, second}}};
    image_is(renderer.render({extent, clear, rectangles}), extent,
        [=](std::uint32_t x, std::uint32_t y) {
            if (x >= 6 && x < 12 && y >= 3 && y < 7) return second;
            if (x >= 2 && x < 14 && y >= 2 && y < 10) return first;
            return clear;
        });
    const std::array<gfx::triangle, 4> reverse{{rectangles[2], rectangles[3],
                                              rectangles[0], rectangles[1]}};
    image_is(renderer.render({extent, clear, reverse}), extent,
        [=](std::uint32_t x, std::uint32_t y) {
            return x >= 2 && x < 14 && y >= 2 && y < 10 ? first : clear;
        });
    // A diagonal through pixel centers has exactly one covering triangle.
    // Either tie owner is allowed, but reversing draw order must not change it:
    // that would expose double coverage. Non-edge pixels have exact oracles.
    const std::array<gfx::triangle, 2> adjacent{{
        {{{{2, 2}, {10, 2}, {2, 10}}}, first},
        {{{{10, 10}, {10, 2}, {2, 10}}}, second}}};
    const std::array<gfx::triangle, 2> adjacent_reverse{{adjacent[1], adjacent[0]}};
    const auto edge_frame = renderer.render({extent, clear, adjacent});
    const auto edge_reverse = renderer.render({extent, clear, adjacent_reverse});
    check(edge_frame.has_value() && edge_reverse.has_value(), "shared-edge renders");
    if (edge_frame && edge_reverse) {
        check(edge_frame->pixels.size() == 192 && edge_reverse->pixels.size() == 192,
              "shared-edge pixel count");
        if (edge_frame->pixels.size() == 192 && edge_reverse->pixels.size() == 192) {
            for (std::uint32_t y = 0; y < 12; ++y) for (std::uint32_t x = 0; x < 16; ++x) {
                const auto index = static_cast<std::size_t>(y) * 16 + x;
                const auto pixel = edge_frame->pixels[index];
                const bool inside = x >= 2 && x < 10 && y >= 2 && y < 10;
                if (inside && x + y == 11)
                    check(near(pixel, first) || near(pixel, second), "shared-edge no holes");
                else
                    check(near(pixel, !inside ? clear : (x + y < 11 ? first : second)),
                          "shared-edge non-tie oracle");
                check(pixel == edge_reverse->pixels[index], "shared-edge single coverage");
            }
        }
    }
    // Changed extent and clear prove there is no previous-frame pixel leakage.
    image_is(renderer.render({{3, 5}, second, {}}), {3, 5},
             [second](auto, auto) { return second; });
    check(renderer.render({{0, 12}, clear, {}}) ==
          std::unexpected(gfx::graphics_error::invalid_extent), "zero extent rejected");
    const gfx::triangle invalid{{{{-1, 0}, {3, 0}, {0, 3}}}, first};
    check(renderer.render({extent, clear, {&invalid, 1}}) ==
          std::unexpected(gfx::graphics_error::invalid_command), "invalid vertex rejected");
    const gfx::triangle degenerate{{{{1, 1}, {2, 2}, {3, 3}}}, first};
    check(renderer.render({extent, clear, {&degenerate, 1}}) ==
          std::unexpected(gfx::graphics_error::invalid_command), "zero area rejected");
    image_is(renderer.render({extent, clear, {}}), extent,
             [clear](auto, auto) { return clear; });
    check(renderer.validation_errors() == 0, "no validation errors during rendering");
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--expect-no-validation") {
        const auto result = gfx::make_vulkan_renderer({true, 0});
        if (result != std::unexpected(gfx::graphics_error::unsupported)) {
            std::cerr << "FAIL: missing required validation layer was not rejected as unsupported\n";
            return 1;
        }
        std::cout << "graphics-validation-unavailable PASS\n";
        return 0;
    }
    std::uint32_t device_index{};
    bool require_validation=true;
    if (argc > 2) return 2;
    if (argc == 2) {
        const std::string_view argument{argv[1]};
        if (argument=="--without-validation") require_validation=false;
        else {
            const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), device_index);
            if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size()) return 2;
        }
    }
    auto unavailable = gfx::make_vulkan_renderer({require_validation, std::numeric_limits<std::uint32_t>::max()});
    check(unavailable == std::unexpected(gfx::graphics_error::unavailable),
          "invalid device index rejected as unavailable");
    {
        // Exercise the production defaults independently of validation-enabled
        // acceptance owners. Absence of diagnostics here is not validation proof.
        auto production = gfx::make_vulkan_renderer();
        check(production.has_value(), "production default factory");
        if (production) {
            constexpr gfx::rgba8 clear{23, 71, 149, 203};
            image_is((*production)->render({{5, 3}, clear, {}}), {5, 3},
                     [clear](auto, auto) { return clear; });
            check((*production)->close().has_value(), "production default close");
        }
    }
    // Each fresh owner runs the corpus twice: independently initialized replay
    // and reuse must satisfy the same oracle, not merely equal each other.
    for (unsigned fresh = 0; fresh < 2; ++fresh) {
        auto created = gfx::make_vulkan_renderer({require_validation, device_index});
        check(created.has_value(), "requested Vulkan mode creation");
        if (!created) {
            std::cerr << "factory error=" << static_cast<int>(created.error()) << '\n';
            continue;
        }
        auto& renderer = **created;
        corpus(renderer);
        corpus(renderer);
        check(renderer.close().has_value(), "explicit close");
        check(renderer.close().has_value(), "idempotent close");
        check(renderer.render({{1, 1}, {}, {}}) ==
              std::unexpected(gfx::graphics_error::invalid_state), "render after close");
        check(renderer.validation_errors() == 0, "no validation errors including teardown");
    }
    // Only asserted semantic facts are emitted, never device names or raw handles.
    if (failures == 0) {
        if (!require_validation) {
            std::cout << "graphics-device-corpus fresh=2 repetitions=2 validation=DISABLED checks=" << checks << '\n';
            return 0;
        }
        std::cout << "graphics-replay-v1 default-factory=verified fresh=2 repetitions=2 clear=verified "
                     "rectangle=verified origin=verified shared-edge=verified draw-order=verified invalid=verified "
                     "close=verified validation-errors=0 checks=" << checks << '\n';
    }
    return failures == 0 ? 0 : 1;
}
