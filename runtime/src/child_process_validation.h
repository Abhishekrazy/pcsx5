#pragma once
#include <pcsx5/runtime/child_process.h>
namespace pcsx5::runtime::detail {
inline bool valid_child_request(const std::filesystem::path& executable,
    std::span<const std::string> arguments, std::uint32_t timeout_ms) noexcept {
    const auto& path = executable.native();
    if (path.empty() || !executable.is_absolute() ||
        path.find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos) return false;
    if (timeout_ms == 0 || timeout_ms > 60000 || arguments.size() > 32 || path.size() > 8192) return false;
    auto total = path.size();
    for (const auto& argument : arguments) {
        if (argument.find('\0') != std::string::npos || argument.size() > 8192 - total) return false;
        total += argument.size();
    }
    return true;
}
} // namespace pcsx5::runtime::detail
