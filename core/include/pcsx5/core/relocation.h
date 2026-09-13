#pragma once

#include <pcsx5/core/dynamic_info.h>
#include <pcsx5/core/guest_memory.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace pcsx5::core {

enum class relocation_error {
    none,
    unmapped_target,
    write_failed,
    unsupported_relocation,
    symbol_resolution_failed,
    overflow,
};

// Callback to resolve a symbol's virtual address by symbol index.
using symbol_resolver = std::expected<std::uint64_t, relocation_error> (*)(
    std::uint32_t symbol_index, void* context) noexcept;

// Applies RELA relocations to guest_memory given the loaded module's base address.
[[nodiscard]] std::expected<std::size_t, relocation_error> apply_relocations(
    std::span<const elf64_rela> relocations,
    std::uint64_t base_address,
    guest_memory& memory,
    symbol_resolver resolver = nullptr,
    void* resolver_context = nullptr) noexcept;

} // namespace pcsx5::core
