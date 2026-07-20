#pragma once
//
// Software fallback for AMD-only Zen 2 instructions (port of SharpEmu
// Sse4aBitFieldEmulator.cs + DirectExecutionBackend.Amd64Compat.cs, commit
// 8ef5a54 "cpu: emulate AMD-only Zen 2 instructions in software (#449)").
//
// PS5 titles occasionally emit AMD-only instructions that Intel hosts do not
// implement, raising #UD (STATUS_ILLEGAL_INSTRUCTION) instead of executing:
//   - SSE4a EXTRQ/INSERTQ, immediate form
//   - MONITORX/MWAITX
//
// The bit-field math and the byte-level decoder are pure (no CONTEXT/SEH
// plumbing) so they can be unit-tested in isolation; the VEH adapter that
// applies a decoded instruction to the live thread context lives in
// src/kernel/kernel.cpp next to the other exception-recovery paths.
//

#include "../common/types.h"
#include <cstddef>

namespace CpuCore::AmdCompat {

// Kind of recoverable AMD-only instruction recognized by Decode().
enum class InstructionKind {
    None,
    Extrq,    // 66 [REX.B] 0F 78 /0 ib(len) ib(idx)   — EXTRQ xmm, imm8, imm8
    Insertq,  // F2 [REX.R/B] 0F 78 /r ib(len) ib(idx)  — INSERTQ xmm, xmm, imm8, imm8
    Monitorx, // 0F 01 FA
    Mwaitx,   // 0F 01 FB
};

// Decoded form of one recoverable instruction at the faulting RIP.
struct DecodedInstruction {
    InstructionKind kind = InstructionKind::None;
    u8 length = 0;     // total instruction byte length, for advancing RIP
    u8 dest_xmm = 0;   // EXTRQ/INSERTQ destination register index (0..15)
    u8 src_xmm = 0;    // INSERTQ source register index (0..15)
    u8 field_len = 0;  // EXTRQ/INSERTQ immediate: field length (low 6 bits used)
    u8 field_idx = 0;  // EXTRQ/INSERTQ immediate: field index  (low 6 bits used)
};

// Attempts to decode one of the recoverable AMD-only instructions from
// `bytes` (up to `size` readable bytes at the faulting RIP).  Returns false
// for anything else — the caller must fall through to the crash path.
bool Decode(const u8* bytes, size_t size, DecodedInstruction* out);

// --- Pure SSE4a bit-field math ---------------------------------------------
// Mirrors the AMD64 manual EXTRQ/INSERTQ immediate-form semantics: a length
// of zero means 64; length/index use only their low 6 bits.

bool IsValidBitField(int length, int index);

// Extracts the [idx, idx+len) bit field of `value` into the low bits.
// Returns 0 for an undefined field (idx+len past bit 63), the full value
// when len == 0 (meaning 64).
u64 ExtractBitField(u64 value, int length, int index);

// Replaces the [idx, idx+len) window of `destination` with the low bits of
// `source`.  Returns `destination` unchanged for an undefined field, the
// full `source` when len == 0 (meaning 64).
u64 InsertBitField(u64 destination, u64 source, int length, int index);

} // namespace CpuCore::AmdCompat
