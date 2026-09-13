#include <pcsx5/core/dynamic_info.h>

#include <cstring>
#include <limits>
#include <optional>

namespace pcsx5::core {

namespace {

[[nodiscard]] std::optional<std::uint64_t> vaddr_to_file_offset(
    std::uint64_t vaddr, std::span<const elf64_phdr> phdrs) noexcept {
    for (const auto& phdr : phdrs) {
        if (phdr.p_type == pt_load) {
            if (vaddr >= phdr.p_vaddr && vaddr < phdr.p_vaddr + phdr.p_filesz) {
                return phdr.p_offset + (vaddr - phdr.p_vaddr);
            }
        }
    }
    return std::nullopt;
}

} // namespace

std::expected<dynamic_info, dynamic_error> parse_dynamic_table(
    const elf64_phdr& dynamic_phdr,
    std::span<const elf64_phdr> all_phdrs,
    std::span<const std::byte> image_bytes) noexcept {
    if (dynamic_phdr.p_offset > image_bytes.size() ||
        dynamic_phdr.p_filesz > image_bytes.size() - dynamic_phdr.p_offset) {
        return std::unexpected(dynamic_error::invalid_dynamic_table);
    }

    const std::size_t num_dyn = dynamic_phdr.p_filesz / sizeof(elf64_dyn);
    if (num_dyn == 0) {
        return std::unexpected(dynamic_error::invalid_dynamic_table);
    }

    dynamic_info info{};
    std::vector<std::uint64_t> needed_strtab_offsets;

    const auto* dyn_ptr = reinterpret_cast<const elf64_dyn*>(image_bytes.data() + dynamic_phdr.p_offset);
    for (std::size_t i = 0; i < num_dyn; ++i) {
        const auto& entry = dyn_ptr[i];
        if (entry.d_tag == dt_null) {
            break;
        }

        switch (entry.d_tag) {
        case dt_needed:
            needed_strtab_offsets.push_back(entry.d_un.d_val);
            break;
        case dt_strtab:
            info.strtab_vaddr = entry.d_un.d_ptr;
            break;
        case dt_strsz:
            info.strtab_size = entry.d_un.d_val;
            break;
        case dt_symtab:
            info.symtab_vaddr = entry.d_un.d_ptr;
            break;
        case dt_syment:
            info.syment = entry.d_un.d_val;
            break;
        case dt_rela:
            info.rela_vaddr = entry.d_un.d_ptr;
            break;
        case dt_relasz:
            info.rela_size = entry.d_un.d_val;
            break;
        case dt_relaent:
            info.rela_ent = entry.d_un.d_val;
            break;
        case dt_jmprel:
            info.jmprel_vaddr = entry.d_un.d_ptr;
            break;
        case dt_pltrelsz:
            info.jmprel_size = entry.d_un.d_val;
            break;
        case dt_init:
            info.init_func = entry.d_un.d_ptr;
            break;
        case dt_fini:
            info.fini_func = entry.d_un.d_ptr;
            break;
        case dt_init_array:
            info.init_array_vaddr = entry.d_un.d_ptr;
            break;
        case dt_init_arraysz:
            info.init_array_size = entry.d_un.d_val;
            break;
        case dt_fini_array:
            info.fini_array_vaddr = entry.d_un.d_ptr;
            break;
        case dt_fini_arraysz:
            info.fini_array_size = entry.d_un.d_val;
            break;
        case dt_preinit_array:
            info.preinit_array_vaddr = entry.d_un.d_ptr;
            break;
        case dt_preinit_arraysz:
            info.preinit_array_size = entry.d_un.d_val;
            break;
        default:
            break;
        }
    }

    // Extract DT_NEEDED strings if strtab is reachable
    if (info.strtab_vaddr != 0) {
        auto str_offset_opt = vaddr_to_file_offset(info.strtab_vaddr, all_phdrs);
        if (str_offset_opt && *str_offset_opt < image_bytes.size()) {
            const std::uint64_t str_offset = *str_offset_opt;
            const std::uint64_t max_str_span = (info.strtab_size > 0)
                ? std::min(info.strtab_size, static_cast<std::uint64_t>(image_bytes.size() - str_offset))
                : static_cast<std::uint64_t>(image_bytes.size() - str_offset);

            const char* strtab_ptr = reinterpret_cast<const char*>(image_bytes.data() + str_offset);
            for (auto off : needed_strtab_offsets) {
                if (off < max_str_span) {
                    const char* str = strtab_ptr + off;
                    std::size_t len = 0;
                    while (off + len < max_str_span && str[len] != '\0') {
                        ++len;
                    }
                    if (off + len < max_str_span) {
                        info.needed_libraries.emplace_back(str, len);
                    }
                }
            }
        }
    }

    // Extract RELA relocations
    if (info.rela_vaddr != 0 && info.rela_size >= sizeof(elf64_rela)) {
        auto rela_offset_opt = vaddr_to_file_offset(info.rela_vaddr, all_phdrs);
        if (rela_offset_opt && *rela_offset_opt < image_bytes.size()) {
            const std::uint64_t rela_offset = *rela_offset_opt;
            const std::uint64_t avail_bytes = image_bytes.size() - rela_offset;
            const std::uint64_t total_rela_bytes = std::min(info.rela_size, avail_bytes);
            const std::size_t count = total_rela_bytes / sizeof(elf64_rela);

            info.relocations.resize(count);
            std::memcpy(info.relocations.data(), image_bytes.data() + rela_offset, count * sizeof(elf64_rela));
        }
    }

    // Extract PLT relocations (JMPREL)
    if (info.jmprel_vaddr != 0 && info.jmprel_size >= sizeof(elf64_rela)) {
        auto jmp_offset_opt = vaddr_to_file_offset(info.jmprel_vaddr, all_phdrs);
        if (jmp_offset_opt && *jmp_offset_opt < image_bytes.size()) {
            const std::uint64_t jmp_offset = *jmp_offset_opt;
            const std::uint64_t avail_bytes = image_bytes.size() - jmp_offset;
            const std::uint64_t total_jmp_bytes = std::min(info.jmprel_size, avail_bytes);
            const std::size_t count = total_jmp_bytes / sizeof(elf64_rela);

            info.plt_relocations.resize(count);
            std::memcpy(info.plt_relocations.data(), image_bytes.data() + jmp_offset, count * sizeof(elf64_rela));
        }
    }

    return info;
}

} // namespace pcsx5::core
