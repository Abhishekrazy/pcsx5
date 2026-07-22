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

// --- Pure BMI1 / BMI2 / ABM operations -----------------------------------
u64 EmulateAndn(u64 src1, u64 src2, u8 operand_size) {
    const u64 res = (~src1) & src2;
    return operand_size == 32 ? (res & 0xFFFFFFFFu) : res;
}

u64 EmulateBlsi(u64 src, u8 operand_size) {
    const u64 res = (-static_cast<s64>(src)) & src;
    return operand_size == 32 ? (res & 0xFFFFFFFFu) : res;
}

u64 EmulateBlsmsk(u64 src, u8 operand_size) {
    const u64 res = (src - 1) ^ src;
    return operand_size == 32 ? (res & 0xFFFFFFFFu) : res;
}

u64 EmulateBlsr(u64 src, u8 operand_size) {
    const u64 res = (src - 1) & src;
    return operand_size == 32 ? (res & 0xFFFFFFFFu) : res;
}

u64 EmulateBextr(u64 src, u64 control, u8 operand_size) {
    const u32 start = control & 0xFF;
    const u32 len = (control >> 8) & 0xFF;
    const u32 max_bits = operand_size == 32 ? 32 : 64;
    if (start >= max_bits) return 0;
    const u32 actual_len = (start + len > max_bits) ? (max_bits - start) : len;
    if (actual_len == 0) return 0;
    const u64 mask = (actual_len == 64) ? ~0ULL : ((1ULL << actual_len) - 1);
    return (src >> start) & mask;
}

u64 EmulateBzhi(u64 src, u64 index, u8 operand_size) {
    const u32 idx = index & 0xFF;
    const u32 max_bits = operand_size == 32 ? 32 : 64;
    if (idx >= max_bits) {
        return operand_size == 32 ? (src & 0xFFFFFFFFu) : src;
    }
    const u64 mask = (1ULL << idx) - 1;
    return src & mask;
}

u64 EmulateTzcnt(u64 src, u8 operand_size) {
    const u32 max_bits = operand_size == 32 ? 32 : 64;
    u64 val = operand_size == 32 ? (src & 0xFFFFFFFFu) : src;
    if (val == 0) return max_bits;
    u32 count = 0;
    while ((val & 1) == 0) {
        count++;
        val >>= 1;
    }
    return count;
}

u64 EmulateLzcnt(u64 src, u8 operand_size) {
    const u32 max_bits = operand_size == 32 ? 32 : 64;
    const u64 val = operand_size == 32 ? (src & 0xFFFFFFFFu) : src;
    if (val == 0) return max_bits;
    u32 count = 0;
    const u64 top_bit = 1ULL << (max_bits - 1);
    u64 mask = top_bit;
    while ((val & mask) == 0) {
        count++;
        mask >>= 1;
    }
    return count;
}

u64 EmulateRorx(u64 src, u8 shift, u8 operand_size) {
    if (operand_size == 32) {
        const u32 val = static_cast<u32>(src);
        const u32 sh = shift & 31;
        if (sh == 0) return val;
        return (val >> sh) | (val << (32 - sh));
    } else {
        const u32 sh = shift & 63;
        if (sh == 0) return src;
        return (src >> sh) | (src << (64 - sh));
    }
}

u64 EmulateSarx(u64 src, u64 shift, u8 operand_size) {
    if (operand_size == 32) {
        const s32 val = static_cast<s32>(src);
        const u32 sh = static_cast<u32>(shift) & 31;
        return static_cast<u32>(val >> sh);
    } else {
        const s64 val = static_cast<s64>(src);
        const u32 sh = static_cast<u32>(shift) & 63;
        return static_cast<u64>(val >> sh);
    }
}

u64 EmulateShlx(u64 src, u64 shift, u8 operand_size) {
    if (operand_size == 32) {
        const u32 val = static_cast<u32>(src);
        const u32 sh = static_cast<u32>(shift) & 31;
        return val << sh;
    } else {
        const u32 sh = static_cast<u32>(shift) & 63;
        return src << sh;
    }
}

u64 EmulateShrx(u64 src, u64 shift, u8 operand_size) {
    if (operand_size == 32) {
        const u32 val = static_cast<u32>(src);
        const u32 sh = static_cast<u32>(shift) & 31;
        return val >> sh;
    } else {
        const u32 sh = static_cast<u32>(shift) & 63;
        return src >> sh;
    }
}

u64 EmulatePdep(u64 src, u64 mask, u8 operand_size) {
    u64 result = 0;
    u64 src_mask = 1;
    const u32 max_bits = operand_size == 32 ? 32 : 64;
    for (u32 i = 0; i < max_bits; ++i) {
        if ((mask >> i) & 1) {
            if (src & src_mask) {
                result |= (1ULL << i);
            }
            src_mask <<= 1;
        }
    }
    return result;
}

u64 EmulatePext(u64 src, u64 mask, u8 operand_size) {
    u64 result = 0;
    u64 dst_bit = 1;
    const u32 max_bits = operand_size == 32 ? 32 : 64;
    for (u32 i = 0; i < max_bits; ++i) {
        if ((mask >> i) & 1) {
            if ((src >> i) & 1) {
                result |= dst_bit;
            }
            dst_bit <<= 1;
        }
    }
    return result;
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

    // ABM instructions: TZCNT (F3 [REX] 0F BC /r), LZCNT (F3 [REX] 0F BD /r)
    if (bytes[0] == 0xF3 && size >= 3) {
        size_t idx = 1;
        u8 rex = 0;
        if ((bytes[idx] & 0xF0) == 0x40) {
            rex = bytes[idx++];
        }
        if (size >= idx + 3 && bytes[idx] == 0x0F && (bytes[idx + 1] == 0xBC || bytes[idx + 1] == 0xBD)) {
            const bool is_lzcnt = (bytes[idx + 1] == 0xBD);
            const u8 modrm = bytes[idx + 2];
            const u8 mod = modrm >> 6;
            const u8 reg = (modrm >> 3) & 7;
            const u8 rm = modrm & 7;
            DecodedInstruction res{};
            res.kind = is_lzcnt ? InstructionKind::Lzcnt : InstructionKind::Tzcnt;
            res.length = static_cast<u8>(idx + 3);
            res.dest_gpr = static_cast<u8>(reg | ((rex & 0x04) ? 8 : 0));
            res.src1_gpr = static_cast<u8>(rm | ((rex & 0x01) ? 8 : 0));
            res.operand_size = (rex & 0x08) ? 64 : 32;
            res.is_memory_src = (mod != 3);
            *out = res;
            return true;
        }
    }

    // VEX-encoded BMI1 / BMI2 instructions (2-byte VEX 0xC5 or 3-byte VEX 0xC4)
    if ((bytes[0] == 0xC4 || bytes[0] == 0xC5) && size >= 4) {
        u8 map_select = 1; // 0F = 1, 0F38 = 2, 0F3A = 3
        u8 pp = 0;
        u8 vvvv = 0;
        u8 r = 0, b = 0, w = 0;
        size_t payload_idx = 1;

        if (bytes[0] == 0xC5) {
            // 2-byte VEX: C5 [R vvvv L pp]
            const u8 b1 = bytes[1];
            r = (b1 >> 7) & 1;
            vvvv = (b1 >> 3) & 0x0F;
            pp = b1 & 3;
            map_select = 1;
            payload_idx = 2;
        } else {
            // 3-byte VEX: C4 [R X B m-mmmm] [W vvvv L pp]
            const u8 b1 = bytes[1];
            const u8 b2 = bytes[2];
            r = (b1 >> 7) & 1;
            b = (b1 >> 5) & 1;
            map_select = b1 & 0x1F;
            w = (b2 >> 7) & 1;
            vvvv = (b2 >> 3) & 0x0F;
            pp = b2 & 3;
            payload_idx = 3;
        }

        if (payload_idx + 1 < size) {
            const u8 opcode = bytes[payload_idx];
            const u8 modrm = bytes[payload_idx + 1];
            const u8 mod = modrm >> 6;
            const u8 reg = (modrm >> 3) & 7;
            const u8 rm = modrm & 7;

            DecodedInstruction res{};
            res.dest_gpr = static_cast<u8>(reg | (r ? 0 : 8));
            res.src1_gpr = static_cast<u8>(rm | (b ? 0 : 8));
            res.src2_gpr = static_cast<u8>((~vvvv) & 0x0F);
            res.operand_size = w ? 64 : 32;
            res.is_memory_src = (mod != 3);
            res.length = static_cast<u8>(payload_idx + 2);

            if (map_select == 1) { // 0F map
                if (opcode == 0x38) {
                    res.kind = InstructionKind::Andn;
                } else if (opcode == 0xF3) {
                    if (reg == 3) res.kind = InstructionKind::Blsi;
                    else if (reg == 2) res.kind = InstructionKind::Blsmsk;
                    else if (reg == 1) res.kind = InstructionKind::Blsr;
                } else if (opcode == 0xF7) {
                    res.kind = InstructionKind::Bextr;
                } else if (opcode == 0xF5) {
                    res.kind = InstructionKind::Bzhi;
                }
            } else if (map_select == 2) { // 0F38 map
                if (opcode == 0xF5) {
                    res.kind = (pp == 2) ? InstructionKind::Pdep : (pp == 3 ? InstructionKind::Pext : InstructionKind::None);
                } else if (opcode == 0xF7) {
                    if (pp == 0) res.kind = InstructionKind::Shlx;
                    else if (pp == 2) res.kind = InstructionKind::Shrx;
                    else if (pp == 3) res.kind = InstructionKind::Sarx;
                }
            } else if (map_select == 3) { // 0F3A map
                if (opcode == 0xF0 && pp == 2 && payload_idx + 2 < size) {
                    res.kind = InstructionKind::Rorx;
                    res.imm8 = bytes[payload_idx + 2];
                    res.length++;
                }
            }

            if (res.kind != InstructionKind::None) {
                *out = res;
                return true;
            }
        }
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
