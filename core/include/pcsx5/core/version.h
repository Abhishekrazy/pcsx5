#pragma once

#include <string_view>

namespace pcsx5::core {

[[nodiscard]] constexpr std::string_view architecture_name() noexcept {
    return "host-independent-core";
}

} // namespace pcsx5::core
