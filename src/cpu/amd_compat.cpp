//
// Software fallback for AMD-only Zen 2 instructions — see amd_compat.h.
//

#include "amd_compat.h"

namespace CpuCore::AmdCompat {

bool IsValidBitField(int length, int index) {
    const int len = length & 0x3F;
    const int idx = index & 0x3F;
    return (len != 0 || idx == 0) && (len == 0 ? idx == 0 : idx + len <= 64);
}

u64 ExtractBitField(u64 value, int length, int index) {
    const int len = length & 0x3F;
    const int idx = index & 0x3F;
    if (!IsValidBitField(length, index)) {
        return 0;
    }
    if (len == 0) {
        return value;
    }
    const u64 mask = (len == 64) ? ~0ULL : ((1ULL << len) - 1);
    return (value >> idx) & mask;
}

u64 InsertBitField(u64 destination, u64 source, int length, int index) {
    const int len = length & 0x3F;
    const int idx = index & 0x3F;
    if (!IsValidBitField(length, index)) {
        return destination;
    }
    if (len == 0) {
        return source;
    }
    const u64 field_mask = (len == 64) ? ~0ULL : ((1ULL << len) - 1);
    const u64 dest_clear_mask = field_mask << idx;
    const u64 source_field = (source & field_mask) << idx;
    return (destination & ~dest_clear_mask) | source_field;
}

// Decoding notes (SharpEmu used the Iced library; we match the same immediate
// forms by hand — the encodings are fixed and short):
//   EXTRQ  xmm, imm8, imm8:       66 [REX] 0F 78 /0 ib ib   (ModRM mod=3, reg=0)
//   INSERTQ xmm1, xmm2, imm, imm: F2 [REX] 0F 78 /r ib ib   (ModRM mod=3)
//   MONITORX / MWAITX:            0F 01 FA / 0F 01 FB (no ModRM)
// Register-form EXTRQ/INSERTQ (0F 79) is not handled, matching upstream, which
// only recovers the immediate forms.
bool Decode(const u8* bytes, size_t size, DecodedInstruction* out) {
    if (!bytes || !out || size < 3) {
        return false;
    }

    // MONITORX (0F 01 FA) and MWAITX (0F 01 FB): fixed 3-byte encodings.
    if (bytes[0] == 0x0F && bytes[1] == 0x01) {
        if (bytes[2] == 0xFA) {
            *out = DecodedInstruction{InstructionKind::Monitorx, 3, 0, 0, 0, 0};
            return true;
        }
        if (bytes[2] == 0xFB) {
            *out = DecodedInstruction{InstructionKind::Mwaitx, 3, 0, 0, 0, 0};
            return true;
        }
        return false;
    }

    // EXTRQ / INSERTQ immediate forms: legacy prefix, optional REX, 0F 78.
    if (bytes[0] != 0x66 && bytes[0] != 0xF2) {
        return false;
    }
    const bool is_extrq = (bytes[0] == 0x66);
    size_t i = 1;
    u8 rex = 0;
    if (i < size && (bytes[i] & 0xF0) == 0x40) {
        rex = bytes[i];
        i++;
    }
    if (size - i < 5) {
        // Need 0F 78 ModRM ib ib after the prefixes.
        return false;
    }
    if (bytes[i] != 0x0F || bytes[i + 1] != 0x78) {
        return false;
    }
    const u8 modrm = bytes[i + 2];
    const u8 mod = modrm >> 6;
    const u8 reg = (modrm >> 3) & 7;
    const u8 rm = modrm & 7;
    if (mod != 3) {
        return false; // memory form — only the register (immediate) form is recoverable
    }
    if (is_extrq && reg != 0) {
        return false; // EXTRQ is /0; a non-zero reg field is a different encoding
    }

    DecodedInstruction result{};
    result.kind = is_extrq ? InstructionKind::Extrq : InstructionKind::Insertq;
    result.length = static_cast<u8>(i + 5);
    result.dest_xmm = static_cast<u8>(rm | ((rex & 0x01) ? 8 : 0)); // REX.B extends rm
    result.src_xmm = static_cast<u8>(reg | ((rex & 0x04) ? 8 : 0)); // REX.R extends reg
    result.field_len = bytes[i + 3];
    result.field_idx = bytes[i + 4];
    *out = result;
    return true;
}

} // namespace CpuCore::AmdCompat
