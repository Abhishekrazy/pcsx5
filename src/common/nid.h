#pragma once
//
// PS5 NID (Name ID) decoder.
//
// Real PS5 game executables (e_type 0xFE10, eboot.bin inner ELF) do not
// carry plain symbol names in their dynamic symbol table.  Instead each
// symbol is identified by an 8-byte value that is then base64-encoded
// using a Sony-specific alphabet and suffixed with a 4-character type
// tag.  The on-the-wire form looks like:
//
//     +P6FRGH4LfA#T#T    <- a function import   (11 NID + 4 tag = 15 chars)
//     -L+-8F0+gBc#A#B    <- a data import
//     1G3lF1Gg1k8#S#N    <- an object (TLS / global data)
//     2Tb92quprl0#B#C    <- an unknown / block type
//
// 8 bytes is 64 bits, which encodes to 11 base64 chars (66-bit slot,
// last 2 bits are padding and ignored).
//
// This header exposes the primitive encode/decode routines plus a small
// name-resolution table covering the most common libkernel / libScePad
// / libSceVideoOut / libSceAgc / libc / libSceFiber symbols that appear
// in retail games.  The database is intentionally small; the goal is
// to make import dumps human-readable, not to be exhaustive.
//
#include "types.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Common {

// Sony's base64 alphabet.  This is *almost* RFC 4648, with the last
// two characters `+` and `/` replaced by `-` and `+` respectively
// (i.e. a "URL-safe" base64 alphabet, with `+`/`-` swapped relative to
// the more common URL-safe variant).
constexpr std::string_view kPs5Base64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";

// 11 base64 chars and 4 type-tag chars.
constexpr size_t kPs5NidEncodedLen   = 11;
constexpr size_t kPs5NidTagLen        = 4;
constexpr size_t kPs5NidFullLen       = kPs5NidEncodedLen + kPs5NidTagLen;  // 15
constexpr size_t kPs5NidRawBytes      = 8;

// 8 raw bytes - the underlying Sony NID value.  This is the actual
// identifier; the base64 representation is just a display encoding.
struct Ps5Nid {
    std::array<u8, kPs5NidRawBytes> bytes{};

    bool operator==(const Ps5Nid& o) const { return bytes == o.bytes; }
    bool operator!=(const Ps5Nid& o) const { return bytes != o.bytes; }
    bool operator<(const Ps5Nid& o) const  { return bytes <  o.bytes; }
};

// The four-character type tag that follows every NID in a real PS5
// game ELF.  The encoded form is "NID#T#T" where the tag is exactly
// 4 chars: leading '#', type char, '#', type char.
enum class Ps5NidType : u8 {
    Unknown = 0,
    Function,   // #T#T
    Data,       // #A#B
    Object,     // #S#N  (TLS / global object)
    Block,      // #B#C
};

std::string_view NidTypeToString(Ps5NidType t) noexcept;
std::optional<Ps5NidType> NidTypeFromString(std::string_view tag) noexcept;

// Decode an 11-character base64 string into an 8-byte Ps5Nid.  Returns
// std::nullopt if `s` is not exactly 11 chars long or contains a
// character outside the Sony alphabet.
std::optional<Ps5Nid> DecodeNid(std::string_view s) noexcept;

// Encode an 8-byte Ps5Nid into an 11-character base64 string.
std::string EncodeNid(const Ps5Nid& nid) noexcept;

// Split a raw dynamic-string import (e.g. "+P6FRGH4LfA#T#T") into the
// 11-char NID and the 4-char type tag.  Returns false if the string
// is not a valid PS5 NID form.
struct ParsedNid {
    Ps5Nid       nid;
    Ps5NidType   type = Ps5NidType::Unknown;
    std::string  raw_tag;     // the original 4-char suffix, e.g. "#T#T"
};

std::optional<ParsedNid> ParseNidString(std::string_view s) noexcept;

// Look up a NID in the built-in name table.  Returns the function/data
// name (e.g. "sceKernelLoadStartModule") on hit, std::nullopt on miss.
//
// The table covers the most common libkernel / libScePad /
// libSceVideoOut / libSceAgc / libc / libSceFiber symbols.  Anything
// not in the table is shown by its raw base64 form.
std::optional<std::string_view> LookupNidName(const Ps5Nid& nid) noexcept;

// True if a NID string (with or without the type suffix) is a known
// name in the table.
bool IsKnownNid(std::string_view nid_with_suffix) noexcept;

}  // namespace Common
