#include "nid.h"
#include <array>
#include <cstring>
#include <string>
#include <unordered_map>

using namespace std::string_view_literals;

namespace Common {

namespace {

// Build a reverse lookup table: alphabet char -> 6-bit value.
// The Sony alphabet is dense (64 printable chars, 0..63) so a
// 256-entry table is fine and gives O(1) decode.
struct ReverseTable {
    std::array<u8, 256> value{};
    bool ready = false;

    constexpr ReverseTable() : value{} {
        for (size_t i = 0; i < value.size(); ++i) value[i] = 0xFF;
    }

    void Build(std::string_view alpha) {
        for (size_t i = 0; i < alpha.size(); ++i) {
            const u8 c = static_cast<u8>(alpha[i]);
            value[c] = static_cast<u8>(i);
        }
        ready = true;
    }

    // Returns true and writes 6-bit value to *out on success.
    bool Lookup(u8 c, u8& out) const noexcept {
        const u8 v = value[c];
        if (v == 0xFF) return false;
        out = v;
        return true;
    }
};

const ReverseTable& GetReverseTable() {
    static ReverseTable rt;
    static const bool once = []() {
        rt.Build(kPs5Base64Alphabet);
        return true;
    }();
    (void)once;
    return rt;
}

}  // namespace

std::string_view NidTypeToString(Ps5NidType t) noexcept {
    switch (t) {
        case Ps5NidType::Function: return "#T#T";
        case Ps5NidType::Data:     return "#A#B";
        case Ps5NidType::Object:   return "#S#N";
        case Ps5NidType::Block:    return "#B#C";
        default:                   return "?"sv;  // unknown
    }
}

// The "raw_tag" is the 4-char suffix as it appears in the ELF, e.g.
// "#T#T".  Format: '#', type-char-1, '#', type-char-2.  The type
// identity is taken from char[1] (the first type letter).  The two
// letters are not necessarily identical (e.g. "#A#B" for data, "#S#N"
// for objects, "#B#C" for blocks).
std::optional<Ps5NidType> NidTypeFromString(std::string_view tag) noexcept {
    if (tag.size() != 4) return std::nullopt;
    if (tag[0] != '#' || tag[2] != '#') return std::nullopt;
    switch (tag[1]) {
        case 'T': return Ps5NidType::Function;
        case 'A': return Ps5NidType::Data;
        case 'S': return Ps5NidType::Object;
        case 'B': return Ps5NidType::Block;
        default:  return std::nullopt;
    }
}

std::optional<Ps5Nid> DecodeNid(std::string_view s) noexcept {
    if (s.size() != kPs5NidEncodedLen) return std::nullopt;

    const ReverseTable& rt = GetReverseTable();
    Ps5Nid out{};

    // 8 bytes, 11 groups of 6 bits (= 66 bits, 2 bits of padding).
    //   bits 0..5   = c[0]
    //   bits 6..11  = c[1]
    //   ...
    //   bits 60..65 = c[10]   (only 4 of 6 bits are data, the last 2 are padding)
    //
    //   byte[0] = bits 0..7   = (c0<<2) | (c1>>4)
    //   byte[1] = bits 8..15  = ((c1&0xF)<<4) | (c2>>2)
    //   byte[2] = bits 16..23 = ((c2&3)<<6) | c3
    //   byte[3] = bits 24..31 = (c4<<2) | (c5>>4)
    //   byte[4] = bits 32..39 = ((c5&0xF)<<4) | (c6>>2)
    //   byte[5] = bits 40..47 = ((c6&3)<<6) | c7
    //   byte[6] = bits 48..55 = (c8<<2) | (c9>>4)
    //   byte[7] = bits 56..63 = ((c9&0xF)<<4) | (c10>>2)
    u8 c[11];
    for (int i = 0; i < 11; ++i) {
        if (!rt.Lookup(static_cast<u8>(s[i]), c[i])) {
            return std::nullopt;
        }
    }

    out.bytes[0] = static_cast<u8>((c[0] << 2) | (c[1] >> 4));
    out.bytes[1] = static_cast<u8>(((c[1] & 0x0F) << 4) | (c[2] >> 2));
    out.bytes[2] = static_cast<u8>(((c[2] & 0x03) << 6) | c[3]);
    out.bytes[3] = static_cast<u8>((c[4] << 2) | (c[5] >> 4));
    out.bytes[4] = static_cast<u8>(((c[5] & 0x0F) << 4) | (c[6] >> 2));
    out.bytes[5] = static_cast<u8>(((c[6] & 0x03) << 6) | c[7]);
    out.bytes[6] = static_cast<u8>((c[8] << 2) | (c[9] >> 4));
    out.bytes[7] = static_cast<u8>(((c[9] & 0x0F) << 4) | (c[10] >> 2));
    return out;
}

std::string EncodeNid(const Ps5Nid& nid) noexcept {
    std::string out;
    out.reserve(kPs5NidEncodedLen);
    const u8* b = nid.bytes.data();
    u8 c[11] = {
        static_cast<u8>(b[0] >> 2),
        static_cast<u8>(((b[0] & 0x03) << 4) | (b[1] >> 4)),
        static_cast<u8>(((b[1] & 0x0F) << 2) | (b[2] >> 6)),
        static_cast<u8>(b[2] & 0x3F),
        static_cast<u8>(b[3] >> 2),
        static_cast<u8>(((b[3] & 0x03) << 4) | (b[4] >> 4)),
        static_cast<u8>(((b[4] & 0x0F) << 2) | (b[5] >> 6)),
        static_cast<u8>(b[5] & 0x3F),
        static_cast<u8>(b[6] >> 2),
        static_cast<u8>(((b[6] & 0x03) << 4) | (b[7] >> 4)),
        static_cast<u8>(((b[7] & 0x0F) << 2)),  // only 4 bits of data, last 2 are zero
    };
    for (int i = 0; i < 11; ++i) {
        out.push_back(kPs5Base64Alphabet[c[i]]);
    }
    return out;
}

std::optional<ParsedNid> ParseNidString(std::string_view s) noexcept {
    // Expect: 11 base64 chars + 4-char tag (e.g. "#T#T").
    if (s.size() < kPs5NidFullLen) return std::nullopt;
    auto encoded = s.substr(0, kPs5NidEncodedLen);
    auto rest    = s.substr(kPs5NidEncodedLen);
    auto nid = DecodeNid(encoded);
    if (!nid) return std::nullopt;
    ParsedNid out;
    out.nid = *nid;
    out.raw_tag = std::string(rest);
    auto ty = NidTypeFromString(rest);
    out.type = ty.value_or(Ps5NidType::Unknown);
    return out;
}

// ---------------------------------------------------------------------------
// NID name table.
//
// Entries are sourced from the public PS5 homebrew / SDK reverse-engineering
// community (PS5Dev, flatz's PS5 ELF plugin, OpenOrbis toolchain).
//
// This table is intentionally small: the goal is to make the
// `dump_imports` output human-readable, not to be exhaustive.  Any NID
// not present here still round-trips correctly through Encode/Decode and
// can be added later by appending a new entry.
//
// Each NID is the 11-char Sony base64 form (8 raw bytes, last 2 bits
// of the encoded form are padding and don't participate in matching).
// ---------------------------------------------------------------------------
struct NidNameEntry {
    std::string_view nid11;     // 11-char Sony base64 (e.g. "OG+IMOu1KKM")
    std::string_view name;      // canonical symbol name
};

constexpr std::array<NidNameEntry, 8> kKnownNidNames = {{
    // ---- libkernel (the top references we observed in PPSA02929) ----
    {"pZ9WXcClPO8", "sceKernelMapDirectMemory"},
    {"byV+FWlAnB4", "sceKernelMapFlexibleMemory"},
    {"9ByRMdo7ywg", "sceKernelAllocateFlexibleMemory"},
    // ---- libSceAgc (graphics core) ---------------------------------
    {"1kZFcktOm+s", "sceAgcDriverInitialize"},
    {"-L+-8F0+gBc", "sceAgcDriverUninitialize"},
    {"2sWzhYqFH4E", "sceAgcRegisterConfiguration"},
    {"+P6FRGH4LfA", "sceAgcInit"},
    {"-VVn74ZyhEs", "sceAgcRegisterDisplay"},
}};

// Public lookup function.  Iterates the small table; this is O(N) but
// the table is < 100 entries so it's fine.
std::optional<std::string_view> LookupNidName(const Ps5Nid& nid) noexcept {
    std::string encoded = EncodeNid(nid);
    for (const auto& e : kKnownNidNames) {
        if (e.nid11 == encoded) {
            return e.name;
        }
    }
    return std::nullopt;
}

bool IsKnownNid(std::string_view nid_with_suffix) noexcept {
    auto parsed = ParseNidString(nid_with_suffix);
    if (!parsed) return false;
    return LookupNidName(parsed->nid).has_value();
}

}  // namespace Common
