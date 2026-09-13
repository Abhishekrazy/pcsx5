#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pcsx5::core {

inline constexpr std::string_view ps5_base64_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";

inline constexpr std::size_t ps5_nid_encoded_len = 11;
inline constexpr std::size_t ps5_nid_tag_len = 4;
inline constexpr std::size_t ps5_nid_full_len = ps5_nid_encoded_len + ps5_nid_tag_len; // 15
inline constexpr std::size_t ps5_nid_raw_bytes = 8;

enum class nid_type : std::uint8_t {
    unknown = 0,
    function,   // #T#T
    data,       // #A#B
    object,     // #S#N
    block       // #B#C
};

struct parsed_nid {
    std::uint64_t nid{0};
    nid_type type{nid_type::unknown};
    std::string raw_tag;
};

// Returns "#T#T", "#A#B", etc.
[[nodiscard]] std::string_view nid_type_to_string(nid_type t) noexcept;

// Decodes a 4-char suffix like "#T#T" into nid_type.
[[nodiscard]] std::optional<nid_type> nid_type_from_string(std::string_view tag) noexcept;

// Decodes an 11-char Sony base64 string into 8 raw bytes.
[[nodiscard]] std::optional<std::array<std::uint8_t, ps5_nid_raw_bytes>> decode_nid_bytes(std::string_view s) noexcept;

// Encodes 8 raw bytes into an 11-char Sony base64 string.
[[nodiscard]] std::string encode_nid_bytes(const std::array<std::uint8_t, ps5_nid_raw_bytes>& bytes) noexcept;

// Decodes an 11-char Sony base64 string into a 64-bit NID integer (little-endian byte mapping).
[[nodiscard]] std::optional<std::uint64_t> decode_nid(std::string_view s) noexcept;

// Encodes a 64-bit NID integer into an 11-char Sony base64 string.
[[nodiscard]] std::string encode_nid(std::uint64_t nid) noexcept;

// Parses a full symbol string (e.g. "+P6FRGH4LfA#T#T" or "188x57JYp0g") into parsed_nid.
[[nodiscard]] std::optional<parsed_nid> parse_nid_string(std::string_view s) noexcept;

// Built-in lookup table for well-known PS5 NID <-> symbol name mappings.
[[nodiscard]] std::optional<std::string_view> lookup_nid_name(std::uint64_t nid) noexcept;
[[nodiscard]] std::optional<std::uint64_t> lookup_name_nid(std::string_view name) noexcept;

} // namespace pcsx5::core
