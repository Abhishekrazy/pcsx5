#include <pcsx5/core/elf_parser.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace pcsx5::core {

bool is_self_format(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < sizeof(self_header)) {
        return false;
    }
    self_header hdr{};
    std::memcpy(&hdr, bytes.data(), sizeof(hdr));
    return hdr.magic == self_magic;
}

std::expected<parsed_elf, elf_error> parse_elf(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < sizeof(elf64_ehdr)) {
        return std::unexpected(elf_error::buffer_too_small);
    }

    elf64_ehdr ehdr{};
    std::memcpy(&ehdr, bytes.data(), sizeof(ehdr));

    // Verify ELF magic
    if (ehdr.e_ident[0] != elfmag0 || ehdr.e_ident[1] != elfmag1 ||
        ehdr.e_ident[2] != elfmag2 || ehdr.e_ident[3] != elfmag3) {
        return std::unexpected(elf_error::invalid_magic);
    }

    // Must be 64-bit ELF
    if (ehdr.e_ident[4] != elfclass64) {
        return std::unexpected(elf_error::unsupported_class);
    }

    // Must be little-endian (2's complement)
    if (ehdr.e_ident[5] != elfdata2lsb) {
        return std::unexpected(elf_error::unsupported_endian);
    }

    // Must be x86-64 machine
    if (ehdr.e_machine != em_x86_64) {
        return std::unexpected(elf_error::unsupported_machine);
    }

    // Validate type: ET_EXEC, ET_DYN, or PS5 SDK module type
    const bool is_dyn = (ehdr.e_type == et_dyn);
    const bool is_ps5 = is_ps5_module_type(ehdr.e_type);
    if (ehdr.e_type != et_exec && !is_dyn && !is_ps5) {
        return std::unexpected(elf_error::unsupported_type);
    }

    // Validate header size
    if (ehdr.e_ehsize != sizeof(elf64_ehdr)) {
        return std::unexpected(elf_error::invalid_header_size);
    }

    // Program headers bounds check
    if (ehdr.e_phnum == 0 || ehdr.e_phentsize != sizeof(elf64_phdr)) {
        return std::unexpected(elf_error::invalid_program_header_table);
    }

    const std::uint64_t file_size = bytes.size();
    if (ehdr.e_phoff > file_size) {
        return std::unexpected(elf_error::invalid_program_header_table);
    }

    const std::uint64_t ph_table_bytes = static_cast<std::uint64_t>(ehdr.e_phnum) * sizeof(elf64_phdr);
    if (ph_table_bytes > file_size - ehdr.e_phoff) {
        return std::unexpected(elf_error::invalid_program_header_table);
    }

    parsed_elf result{};
    result.header = ehdr;
    result.is_pie = is_dyn || is_ps5;
    result.is_ps5_module = is_ps5;
    result.image_bytes = bytes;
    result.program_headers.resize(ehdr.e_phnum);

    std::memcpy(result.program_headers.data(), bytes.data() + ehdr.e_phoff, ph_table_bytes);

    // Validate individual program headers
    for (const auto& phdr : result.program_headers) {
        if (phdr.p_type == pt_load) {
            if (phdr.p_filesz > phdr.p_memsz) {
                return std::unexpected(elf_error::file_size_exceeds_memory_size);
            }
            if (phdr.p_offset > file_size || phdr.p_filesz > file_size - phdr.p_offset) {
                return std::unexpected(elf_error::segment_out_of_bounds);
            }
        }
    }

    // Section headers bounds check (optional in stripped executables)
    if (ehdr.e_shnum > 0) {
        if (ehdr.e_shentsize != sizeof(elf64_shdr) || ehdr.e_shoff > file_size) {
            return std::unexpected(elf_error::invalid_section_header_table);
        }
        const std::uint64_t sh_table_bytes = static_cast<std::uint64_t>(ehdr.e_shnum) * sizeof(elf64_shdr);
        if (sh_table_bytes > file_size - ehdr.e_shoff) {
            return std::unexpected(elf_error::invalid_section_header_table);
        }
        result.section_headers.resize(ehdr.e_shnum);
        std::memcpy(result.section_headers.data(), bytes.data() + ehdr.e_shoff, sh_table_bytes);
    }

    return result;
}

std::expected<std::span<const std::byte>, elf_error> extract_self_inner_elf(
    std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < sizeof(self_header)) {
        return std::unexpected(elf_error::buffer_too_small);
    }

    self_header hdr{};
    std::memcpy(&hdr, bytes.data(), sizeof(hdr));

    if (hdr.magic != self_magic) {
        return std::unexpected(elf_error::invalid_self_header);
    }

    if (hdr.header_size > bytes.size()) {
        return std::unexpected(elf_error::invalid_self_header);
    }

    // For unencrypted/decrypted SELF containers, the inner ELF payload begins
    // directly after the SELF header (at header_size) or is specified by the first segment.
    const std::uint64_t payload_offset = hdr.header_size;
    const std::uint64_t payload_size = (bytes.size() > payload_offset) ? (bytes.size() - payload_offset) : 0;

    // Check if inner payload starts with ELF magic
    if (payload_size >= sizeof(elf64_ehdr)) {
        const auto* ptr = reinterpret_cast<const std::uint8_t*>(bytes.data() + payload_offset);
        if (ptr[0] == elfmag0 && ptr[1] == elfmag1 && ptr[2] == elfmag2 && ptr[3] == elfmag3) {
            return bytes.subspan(payload_offset, payload_size);
        }
    }

    // Otherwise check segment table if present
    if (hdr.segment_count > 0 && hdr.segment_count <= 256) {
        const std::uint64_t seg_table_offset = sizeof(self_header);
        const std::uint64_t seg_table_bytes = hdr.segment_count * sizeof(self_segment_entry);
        if (seg_table_offset + seg_table_bytes <= hdr.header_size &&
            seg_table_offset + seg_table_bytes <= bytes.size()) {
            std::vector<self_segment_entry> segments(static_cast<std::size_t>(hdr.segment_count));
            std::memcpy(segments.data(), bytes.data() + seg_table_offset, seg_table_bytes);
            for (const auto& seg : segments) {
                if (seg.offset > 0 && seg.offset < bytes.size() && (bytes.size() - seg.offset) >= sizeof(elf64_ehdr)) {
                    const auto* ptr = reinterpret_cast<const std::uint8_t*>(bytes.data() + seg.offset);
                    if (ptr[0] == elfmag0 && ptr[1] == elfmag1 && ptr[2] == elfmag2 && ptr[3] == elfmag3) {
                        const std::uint64_t size = std::min(seg.decrypted_uncompressed_size, static_cast<std::uint64_t>(bytes.size() - seg.offset));
                        return bytes.subspan(static_cast<std::size_t>(seg.offset), static_cast<std::size_t>(size));
                    }
                }
            }
        }
    }

    return std::unexpected(elf_error::self_inner_elf_truncated);
}

std::expected<parsed_elf, elf_error> parse_self_or_elf(
    std::span<const std::byte> bytes) noexcept {
    if (is_self_format(bytes)) {
        auto inner_elf = extract_self_inner_elf(bytes);
        if (!inner_elf) {
            return std::unexpected(inner_elf.error());
        }
        return parse_elf(*inner_elf);
    }
    return parse_elf(bytes);
}

} // namespace pcsx5::core
