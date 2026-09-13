#pragma once

#include <pcsx5/core/elf_parser.h>
#include <pcsx5/core/guest_memory.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace pcsx5::core {

enum class loader_error {
    none,
    invalid_elf,
    no_loadable_segments,
    invalid_segment_alignment,
    memory_allocation_failed,
    memory_mapping_failed,
    memory_write_failed,
    segment_address_overflow,
    address_overlap,
};

struct loaded_segment {
    std::uint64_t virtual_address{0};
    std::uint64_t memory_size{0};
    std::uint64_t file_size{0};
    guest_memory_access access{guest_memory_access::none};
    std::uint32_t flags{0};
};

struct tls_template {
    std::uint64_t virtual_address{0};
    std::uint64_t file_size{0};
    std::uint64_t memory_size{0};
    std::uint64_t alignment{0};
    std::span<const std::byte> initialization_image;
};

struct loaded_module {
    std::uint64_t base_address{0};
    std::uint64_t entry_point{0};
    std::uint64_t total_span{0};
    std::vector<loaded_segment> segments;
    std::optional<tls_template> tls;
    parsed_elf parsed;
};

// Factory callback providing an owned memory_backing of at least `size` bytes.
using backing_factory = guest_memory_result<std::unique_ptr<memory_backing>> (*)(std::uint64_t size) noexcept;

// Calculates the virtual address span [min_vaddr, span_size] required by PT_LOAD segments.
[[nodiscard]] std::expected<std::pair<std::uint64_t, std::uint64_t>, loader_error>
calculate_module_span(const parsed_elf& elf) noexcept;

// Maps all PT_LOAD segments into guest_memory and populates the loaded_module record.
[[nodiscard]] std::expected<loaded_module, loader_error> load_module(
    const parsed_elf& elf,
    std::uint64_t preferred_base,
    guest_memory& memory,
    backing_factory allocator) noexcept;

} // namespace pcsx5::core
