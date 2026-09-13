#include <pcsx5/core/nid.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace pcsx5::core {

namespace {

struct reverse_alphabet_table {
    std::array<std::uint8_t, 256> values{};

    constexpr reverse_alphabet_table() {
        for (std::size_t i = 0; i < values.size(); ++i) {
            values[i] = 0xFF;
        }
        for (std::size_t i = 0; i < ps5_base64_alphabet.size(); ++i) {
            values[static_cast<std::uint8_t>(ps5_base64_alphabet[i])] = static_cast<std::uint8_t>(i);
        }
    }

    [[nodiscard]] constexpr bool lookup(std::uint8_t c, std::uint8_t& out) const noexcept {
        const auto v = values[c];
        if (v == 0xFF) {
            return false;
        }
        out = v;
        return true;
    }
};

inline constexpr reverse_alphabet_table k_reverse_table{};

struct known_nid_entry {
    std::string_view nid_str;
    std::string_view name;
};

inline constexpr std::array<known_nid_entry, 16> k_known_nids = {{
    {"L-Q3LEjIbgA", "sceKernelMapDirectMemory"},
    {"IWIBBdTHit4", "sceKernelMapFlexibleMemory"},
    {"xaxE6OHpkiM", "sceKernelAllocateFlexibleMemory"},
    {"188x57JYp0g", "sceKernelCreateSema"},
    {"1kZFcktOm+s", "sceAgcDriverInitialize"},
    {"-L+-8F0+gBc", "sceAgcDriverUninitialize"},
    {"2sWzhYqFH4E", "sceAgcRegisterConfiguration"},
    {"-VVn74ZyhEs", "sceAgcRegisterDisplay"},
    {"+P6FRGH4LfA", "memmove"},
    {"+BzXYkqYeLE", "scePthreadSetspecific"},
    {"+I4K03i3EL0", "sceVideoOutInitializeOutputOptions"},
    {"+L98PIbGttk", "scePthreadRwlockUnlock"},
    {"-2IRUCO--PM", "sceKernelReadTsc"},
    {"-4GCfYdNF1s", "sceImeUpdate"},
    {"-5y2uJ62qS8", "sceRtcTickAddYears"},
    {"-HOOCn0JY48", "sceAgcDcbSetShRegistersIndirect"},
}};

} // namespace

std::string_view nid_type_to_string(nid_type t) noexcept {
    switch (t) {
    case nid_type::function: return "#T#T";
    case nid_type::data:     return "#A#B";
    case nid_type::object:   return "#S#N";
    case nid_type::block:    return "#B#C";
    default:                 return "";
    }
}

std::optional<nid_type> nid_type_from_string(std::string_view tag) noexcept {
    if (tag.size() != 4 || tag[0] != '#' || tag[2] != '#') {
        return std::nullopt;
    }
    switch (tag[1]) {
    case 'T': return nid_type::function;
    case 'A': return nid_type::data;
    case 'S': return nid_type::object;
    case 'B': return nid_type::block;
    default:  return std::nullopt;
    }
}

std::optional<std::array<std::uint8_t, ps5_nid_raw_bytes>> decode_nid_bytes(std::string_view s) noexcept {
    if (s.size() != ps5_nid_encoded_len) {
        return std::nullopt;
    }

    std::array<std::uint8_t, 11> c{};
    for (std::size_t i = 0; i < 11; ++i) {
        if (!k_reverse_table.lookup(static_cast<std::uint8_t>(s[i]), c[i])) {
            return std::nullopt;
        }
    }

    std::array<std::uint8_t, ps5_nid_raw_bytes> out{};
    out[0] = static_cast<std::uint8_t>((c[0] << 2) | (c[1] >> 4));
    out[1] = static_cast<std::uint8_t>(((c[1] & 0x0F) << 4) | (c[2] >> 2));
    out[2] = static_cast<std::uint8_t>(((c[2] & 0x03) << 6) | c[3]);
    out[3] = static_cast<std::uint8_t>((c[4] << 2) | (c[5] >> 4));
    out[4] = static_cast<std::uint8_t>(((c[5] & 0x0F) << 4) | (c[6] >> 2));
    out[5] = static_cast<std::uint8_t>(((c[6] & 0x03) << 6) | c[7]);
    out[6] = static_cast<std::uint8_t>((c[8] << 2) | (c[9] >> 4));
    out[7] = static_cast<std::uint8_t>(((c[9] & 0x0F) << 4) | (c[10] >> 2));
    return out;
}

std::string encode_nid_bytes(const std::array<std::uint8_t, ps5_nid_raw_bytes>& bytes) noexcept {
    const auto* b = bytes.data();
    const std::array<std::uint8_t, 11> c = {{
        static_cast<std::uint8_t>(b[0] >> 2),
        static_cast<std::uint8_t>(((b[0] & 0x03) << 4) | (b[1] >> 4)),
        static_cast<std::uint8_t>(((b[1] & 0x0F) << 2) | (b[2] >> 6)),
        static_cast<std::uint8_t>(b[2] & 0x3F),
        static_cast<std::uint8_t>(b[3] >> 2),
        static_cast<std::uint8_t>(((b[3] & 0x03) << 4) | (b[4] >> 4)),
        static_cast<std::uint8_t>(((b[4] & 0x0F) << 2) | (b[5] >> 6)),
        static_cast<std::uint8_t>(b[5] & 0x3F),
        static_cast<std::uint8_t>(b[6] >> 2),
        static_cast<std::uint8_t>(((b[6] & 0x03) << 4) | (b[7] >> 4)),
        static_cast<std::uint8_t>((b[7] & 0x0F) << 2),
    }};

    std::string out;
    out.reserve(ps5_nid_encoded_len);
    for (std::size_t i = 0; i < 11; ++i) {
        out.push_back(ps5_base64_alphabet[c[i]]);
    }
    return out;
}

std::optional<std::uint64_t> decode_nid(std::string_view s) noexcept {
    const auto bytes_opt = decode_nid_bytes(s);
    if (!bytes_opt) {
        return std::nullopt;
    }
    std::uint64_t val = 0;
    std::memcpy(&val, bytes_opt->data(), sizeof(val));
    return val;
}

std::string encode_nid(std::uint64_t nid) noexcept {
    std::array<std::uint8_t, ps5_nid_raw_bytes> bytes{};
    std::memcpy(bytes.data(), &nid, sizeof(nid));
    return encode_nid_bytes(bytes);
}

std::optional<parsed_nid> parse_nid_string(std::string_view s) noexcept {
    if (s.size() == ps5_nid_encoded_len) {
        auto nid_val = decode_nid(s);
        if (!nid_val) {
            return std::nullopt;
        }
        return parsed_nid{
            .nid = *nid_val,
            .type = nid_type::unknown,
            .raw_tag = {},
        };
    }

    if (s.size() == ps5_nid_full_len) {
        const auto encoded_part = s.substr(0, ps5_nid_encoded_len);
        const auto tag_part = s.substr(ps5_nid_encoded_len);

        auto nid_val = decode_nid(encoded_part);
        if (!nid_val) {
            return std::nullopt;
        }

        auto tag_type = nid_type_from_string(tag_part);
        return parsed_nid{
            .nid = *nid_val,
            .type = tag_type.value_or(nid_type::unknown),
            .raw_tag = std::string(tag_part),
        };
    }

    return std::nullopt;
}

std::optional<std::string_view> lookup_nid_name(std::uint64_t nid) noexcept {
    const std::string encoded = encode_nid(nid);
    for (const auto& entry : k_known_nids) {
        if (entry.nid_str == encoded) {
            return entry.name;
        }
    }
    return std::nullopt;
}

std::optional<std::uint64_t> lookup_name_nid(std::string_view name) noexcept {
    for (const auto& entry : k_known_nids) {
        if (entry.name == name) {
            return decode_nid(entry.nid_str);
        }
    }
    return std::nullopt;
}

} // namespace pcsx5::core
