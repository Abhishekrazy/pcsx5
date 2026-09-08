#pragma once
#include <pcsx5/runtime/memory.h>
#include <cstddef>

namespace pcsx5::runtime::detail {
// Private per-owner test seam. Context outlives owner. Native failures preserve
// errno; callbacks otherwise perform the real operation with native semantics.
struct linux_memory_api {
    void* context{};
    void* (*map)(void*, std::size_t) noexcept{};
    int (*unmap)(void*, void*, std::size_t) noexcept{};
    int (*protect)(void*, void*, std::size_t, int) noexcept{};
    int (*discard)(void*, void*, std::size_t) noexcept{};
};
[[nodiscard]] memory_result<std::unique_ptr<memory_reservation>>
reserve_linux_memory_with_api(std::uint64_t size, linux_memory_api api) noexcept;
} // namespace pcsx5::runtime::detail
