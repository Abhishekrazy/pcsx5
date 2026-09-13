#include <pcsx5/core/loader.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace pcsx5::core {

namespace {

constexpr std::uint64_t kPageSize = 4096;
constexpr std::uint64_t kPageMask = kPageSize - 1;

[[nodiscard]] constexpr std::uint64_t align_down(std::uint64_t val) noexcept {
    return val & ~kPageMask;
}

[[nodiscard]] constexpr std::uint64_t align_up(std::uint64_t val) noexcept {
    return (val + kPageMask) & ~kPageMask;
}

} // namespace

std::expected<std::pair<std::uint64_t, std::uint64_t>, loader_error>
calculate_module_span(const parsed_elf& elf) noexcept {
    bool has_load = false;
    std::uint64_t min_vaddr = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_vaddr = 0;

    for (const auto& phdr : elf.program_headers) {
        if (phdr.p_type != pt_load) {
            continue;
        }

        has_load = true;
        if (phdr.p_memsz > std::numeric_limits<std::uint64_t>::max() - phdr.p_vaddr) {
            return std::unexpected(loader_error::segment_address_overflow);
        }

        const std::uint64_t seg_start = phdr.p_vaddr;
        const std::uint64_t seg_end = phdr.p_vaddr + phdr.p_memsz;

        min_vaddr = std::min(min_vaddr, seg_start);
        max_vaddr = std::max(max_vaddr, seg_end);
    }

    if (!has_load) {
        return std::unexpected(loader_error::no_loadable_segments);
    }

    return std::make_pair(min_vaddr, max_vaddr - min_vaddr);
}

std::expected<loaded_module, loader_error> load_module(
    const parsed_elf& elf,
    std::uint64_t preferred_base,
    guest_memory& memory,
    backing_factory allocator) noexcept {
    if (allocator == nullptr) {
        return std::unexpected(loader_error::memory_allocation_failed);
    }

    auto span_res = calculate_module_span(elf);
    if (!span_res) {
        return std::unexpected(span_res.error());
    }

    const auto [min_vaddr, span_size] = *span_res;
    const std::uint64_t actual_base = elf.is_pie ? preferred_base : 0;

    // Check entry point overflow
    if (elf.header.e_entry > std::numeric_limits<std::uint64_t>::max() - actual_base) {
        return std::unexpected(loader_error::segment_address_overflow);
    }

    loaded_module module{};
    module.base_address = actual_base;
    module.entry_point = actual_base + elf.header.e_entry;
    module.total_span = span_size;
    module.parsed = elf;

    for (const auto& phdr : elf.program_headers) {
        if (phdr.p_type == pt_load) {
            if (phdr.p_memsz == 0) {
                continue;
            }

            if (phdr.p_vaddr > std::numeric_limits<std::uint64_t>::max() - actual_base) {
                return std::unexpected(loader_error::segment_address_overflow);
            }

            const std::uint64_t seg_vaddr = actual_base + phdr.p_vaddr;
            const std::uint64_t map_base = align_down(seg_vaddr);
            const std::uint64_t offset_in_page = seg_vaddr - map_base;

            if (phdr.p_memsz > std::numeric_limits<std::uint64_t>::max() - offset_in_page) {
                return std::unexpected(loader_error::segment_address_overflow);
            }

            const std::uint64_t map_size = align_up(phdr.p_memsz + offset_in_page);

            const guest_memory_access target_access =
                ((phdr.p_flags & pf_w) != 0) ? guest_memory_access::read_write : guest_memory_access::read_only;

            // Allocate backing through the provided factory
            auto backing_res = allocator(map_size);
            if (!backing_res) {
                return std::unexpected(loader_error::memory_allocation_failed);
            }

            auto backing = std::move(*backing_res);
            if (!backing) {
                return std::unexpected(loader_error::memory_allocation_failed);
            }

            // Map segment initially as read_write to populate file contents
            auto map_res = memory.map(map_base, map_size, guest_memory_access::read_write, backing);
            if (!map_res) {
                if (map_res.error() == guest_memory_error::overlap) {
                    return std::unexpected(loader_error::address_overlap);
                }
                return std::unexpected(loader_error::memory_mapping_failed);
            }

            // Write segment file data
            if (phdr.p_filesz > 0) {
                if (phdr.p_offset > elf.image_bytes.size() ||
                    phdr.p_filesz > elf.image_bytes.size() - phdr.p_offset) {
                    return std::unexpected(loader_error::invalid_elf);
                }

                auto file_span = elf.image_bytes.subspan(
                    static_cast<std::size_t>(phdr.p_offset),
                    static_cast<std::size_t>(phdr.p_filesz));

                auto write_res = memory.write(seg_vaddr, file_span);
                if (!write_res) {
                    return std::unexpected(loader_error::memory_write_failed);
                }
            }

            // Apply final read-only access if requested
            if (target_access == guest_memory_access::read_only) {
                auto prot_res = memory.protect(map_base, guest_memory_access::read_only);
                if (!prot_res) {
                    return std::unexpected(loader_error::memory_mapping_failed);
                }
            }

            loaded_segment seg{};
            seg.virtual_address = seg_vaddr;
            seg.memory_size = phdr.p_memsz;
            seg.file_size = phdr.p_filesz;
            seg.access = target_access;
            seg.flags = phdr.p_flags;
            module.segments.push_back(seg);
        } else if (phdr.p_type == pt_tls) {
            tls_template tls{};
            tls.virtual_address = actual_base + phdr.p_vaddr;
            tls.file_size = phdr.p_filesz;
            tls.memory_size = phdr.p_memsz;
            tls.alignment = phdr.p_align;

            if (phdr.p_filesz > 0 && phdr.p_offset <= elf.image_bytes.size() &&
                phdr.p_filesz <= elf.image_bytes.size() - phdr.p_offset) {
                tls.initialization_image = elf.image_bytes.subspan(
                    static_cast<std::size_t>(phdr.p_offset),
                    static_cast<std::size_t>(phdr.p_filesz));
            }

            module.tls = tls;
        }
    }

    return module;
}

} // namespace pcsx5::core
