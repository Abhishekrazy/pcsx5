#pragma once

#include <pcsx5/core/elf_types.h>
#include <pcsx5/core/self_types.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace pcsx5::core {

enum class elf_error {
    none,
    buffer_too_small,
    invalid_magic,
    unsupported_class,
    unsupported_endian,
    unsupported_version,
    unsupported_machine,
    unsupported_type,
    invalid_header_size,
    invalid_program_header_table,
    invalid_section_header_table,
    segment_out_of_bounds,
    file_size_exceeds_memory_size,
    invalid_self_header,
    invalid_self_segments,
    self_inner_elf_truncated,
};

struct parsed_elf {
    elf64_ehdr header{};
    std::vector<elf64_phdr> program_headers;
    std::vector<elf64_shdr> section_headers;
    bool is_pie{false};
    bool is_ps5_module{false};
    std::span<const std::byte> image_bytes;
};

// Returns true if the buffer starts with Sony SELF magic (0x4F4C531D).
[[nodiscard]] bool is_self_format(std::span<const std::byte> bytes) noexcept;

// Parses and validates an ELF64 x86-64 binary image with strict bounds checking.
[[nodiscard]] std::expected<parsed_elf, elf_error> parse_elf(
    std::span<const std::byte> bytes) noexcept;

// Extracts the uncompressed/decrypted inner ELF image from a SELF container.
[[nodiscard]] std::expected<std::span<const std::byte>, elf_error> extract_self_inner_elf(
    std::span<const std::byte> bytes) noexcept;

// Parses either a SELF container (extracting the inner ELF) or a standard ELF64 image.
[[nodiscard]] std::expected<parsed_elf, elf_error> parse_self_or_elf(
    std::span<const std::byte> bytes) noexcept;

} // namespace pcsx5::core
