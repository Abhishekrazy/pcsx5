#pragma once
#include <pcsx5/core/guest_memory.h>
#include <pcsx5/runtime/memory.h>

namespace pcsx5::runtime {
using memory_provider = memory_result<std::unique_ptr<memory_reservation>> (*)(std::uint64_t) noexcept;
// Allocates and commits host RW data storage. Core applies guest permissions.
// Size must satisfy the selected provider's page geometry; no implicit rounding.
[[nodiscard]] core::guest_memory_result<std::unique_ptr<core::memory_backing>>
make_guest_memory_backing(std::uint64_t size, memory_provider provider) noexcept;
} // namespace pcsx5::runtime
