#pragma once
#include "../common/types.h"
#include <string>

namespace InstrDecode {

// Result of decoding one x86-64 instruction.
struct Decoded {
    u32         length = 0;      // total instruction length in bytes (0 if unreadable)
    std::string text;            // "mov rax, [rsp+0x10]"  or  "?? 48 8B 04 24 ..."
    bool        known = false;   // false => text is raw hex ("?? AA BB ...")
};

// Decode at most `size` bytes from `bytes`.  Never throws.  `size` should be
// at least 15 (x86-64 maximum instruction length) for best results; a smaller
// buffer may produce truncated output.  The returned `length` is capped at 15.
Decoded Decode(const u8* bytes, size_t size);

// Decode a single instruction at `bytes` and return a one-line human-readable
// description, e.g. "cmp byte [rbx+0x10], 0x20".  Returns raw hex if unknown.
// Convenience — same as Decode().text but trims trailing whitespace.
std::string DecodeOneLine(const u8* bytes, size_t size);

} // namespace InstrDecode
