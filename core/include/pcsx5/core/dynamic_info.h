#pragma once

#include <pcsx5/core/elf_types.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pcsx5::core {

enum class dynamic_error {
    none,
    invalid_dynamic_table,
    string_table_missing,
    symbol_table_missing,
    relocation_table_out_of_bounds,
    relocation_target_unmapped,
    unsupported_relocation_type,
    symbol_not_found,
};

struct dynamic_info {
    std::vector<std::string> needed_libraries;
    std::uint64_t init_func{0};
    std::uint64_t fini_func{0};
    std::uint64_t init_array_vaddr{0};
    std::uint64_t init_array_size{0};
    std::uint64_t fini_array_vaddr{0};
    std::uint64_t fini_array_size{0};
    std::uint64_t preinit_array_vaddr{0};
    std::uint64_t preinit_array_size{0};

    std::uint64_t rela_vaddr{0};
    std::uint64_t rela_size{0};
    std::uint64_t rela_ent{sizeof(elf64_rela)};

    std::uint64_t jmprel_vaddr{0};
    std::uint64_t jmprel_size{0};

    std::uint64_t strtab_vaddr{0};
    std::uint64_t strtab_size{0};

    std::uint64_t symtab_vaddr{0};
    std::uint64_t syment{sizeof(elf64_sym)};

    std::vector<elf64_rela> relocations;
    std::vector<elf64_rela> plt_relocations;
};

// Parses PT_DYNAMIC segment and extracts dynamic metadata, needed libraries, and relocation records.
[[nodiscard]] std::expected<dynamic_info, dynamic_error> parse_dynamic_table(
    const elf64_phdr& dynamic_phdr,
    std::span<const elf64_phdr> all_phdrs,
    std::span<const std::byte> image_bytes) noexcept;

} // namespace pcsx5::core
