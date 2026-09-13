#pragma once

#include <cstddef>
#include <cstdint>

namespace pcsx5::core {

// Sony SELF container header magic (0x4F4C531D in little endian: "1D 53 4C 4F").
inline constexpr std::uint32_t self_magic = 0x4F4C531D;


struct self_header {
    std::uint32_t magic;         // 0x4F4C531D
    std::uint8_t  version;       // Header version
    std::uint8_t  mode;          // Mode
    std::uint16_t endian;        // 1 = Little Endian
    std::uint32_t attributes;    // Attributes
    std::uint32_t category;      // Category
    std::uint32_t program_type;  // Program type
    std::uint32_t padding;
    std::uint64_t header_size;   // Size of entire SELF metadata header
    std::uint64_t signature_size;
    std::uint64_t file_size;     // Size of the SELF file
    std::uint64_t segment_count; // Number of segment records
    std::uint64_t unknown;
};
static_assert(sizeof(self_header) == 64, "self_header size must be exactly 64 bytes");

struct self_segment_entry {
    std::uint64_t flags;
    std::uint64_t offset;
    std::uint64_t encrypted_compressed_size;
    std::uint64_t decrypted_uncompressed_size;
};
static_assert(sizeof(self_segment_entry) == 32, "self_segment_entry size must be exactly 32 bytes");


} // namespace pcsx5::core

