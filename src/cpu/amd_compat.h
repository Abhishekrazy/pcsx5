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
    // BMI1 / BMI2 / ABM instructions
    Andn,     // VEX.NDS.LZ.0F.W0 38 /r  — ANDN r32/r64, r32/r64, r/m32/r/m64
    Blsi,     // VEX.NDD.LZ.0F.W0 F3 /3  — BLSI r32/r64, r/m32/r/m64
    Blsmsk,   // VEX.NDD.LZ.0F.W0 F3 /2  — BLSMSK r32/r64, r/m32/r/m64
    Blsr,     // VEX.NDD.LZ.0F.W0 F3 /1  — BLSR r32/r64, r/m32/r/m64
    Bextr,    // VEX.NDS.LZ.0F.W0 F7 /r  — BEXTR r32/r64, r/m32/r/m64, r32/r64
    Bzhi,     // VEX.NDS.LZ.0F.W0 F5 /r  — BZHI r32/r64, r/m32/r/m64, r32/r64
    Tzcnt,    // F3 [REX] 0F BC /r       — TZCNT r32/r64, r/m32/r/m64
    Lzcnt,    // F3 [REX] 0F BD /r       — LZCNT r32/r64, r/m32/r/m64
    Rorx,     // VEX.LZ.F2.0F3A.W0 F0 /r ib — RORX r32/r64, r/m32/r/m64, imm8
    Sarx,     // VEX.NDS.LZ.F3.0F38.W0 F7 /r — SARX r32/r64, r/m32/r/m64, r32/r64
    Shlx,     // VEX.NDS.LZ.66.0F38.W0 F7 /r — SHLX r32/r64, r/m32/r/m64, r32/r64
    Shrx,     // VEX.NDS.LZ.F2.0F38.W0 F7 /r — SHRX r32/r64, r/m32/r/m64, r32/r64
    Pdep,     // VEX.NDS.LZ.F2.0F38.W0 F5 /r — PDEP r32/r64, r32/r64, r/m32/r/m64
    Pext,     // VEX.NDS.LZ.F3.0F38.W0 F5 /r — PEXT r32/r64, r32/r64, r/m32/r/m64
};

struct DecodedInstruction {
    InstructionKind kind = InstructionKind::None;
    u8 length = 0;     // total instruction byte length, for advancing RIP
    u8 dest_xmm = 0;   // EXTRQ/INSERTQ destination register index (0..15)
    u8 src_xmm = 0;    // INSERTQ source register index (0..15)
    u8 field_len = 0;  // EXTRQ/INSERTQ immediate: field length (low 6 bits used)
    u8 field_idx = 0;  // EXTRQ/INSERTQ immediate: field index  (low 6 bits used)
    // BMI1 / BMI2 / ABM GPR fields
    u8 dest_gpr = 0;   // destination GPR index (0..15)
    u8 src1_gpr = 0;   // first source GPR index (0..15)
    u8 src2_gpr = 0;   // second source GPR index / VEX.vvvv (0..15)
    u8 operand_size = 32; // 32 or 64 bits (REX.W / VEX.W)
    u8 imm8 = 0;        // immediate byte (RORX)
    bool is_memory_src = false; // true if src1/src2 is memory operand
    u64 mem_addr = 0;   // guest address when is_memory_src is true
};

// Attempts to decode one of the recoverable AMD-only instructions from
// `bytes` (up to `size` readable bytes at the faulting RIP).  Returns false
// for anything else — the caller must fall through to the crash path.
bool Decode(const u8* bytes, size_t size, DecodedInstruction* out);

// --- Pure SSE4a & BMI bit-field / bit manipulation math -------------------
bool IsValidBitField(int length, int index);
u64 ExtractBitField(u64 value, int length, int index);
u64 InsertBitField(u64 destination, u64 source, int length, int index);

// BMI1/BMI2/ABM pure operations (64-bit / 32-bit width awareness)
u64 EmulateAndn(u64 src1, u64 src2, u8 operand_size);
u64 EmulateBlsi(u64 src, u8 operand_size);
u64 EmulateBlsmsk(u64 src, u8 operand_size);
u64 EmulateBlsr(u64 src, u8 operand_size);
u64 EmulateBextr(u64 src, u64 control, u8 operand_size);
u64 EmulateBzhi(u64 src, u64 index, u8 operand_size);
u64 EmulateTzcnt(u64 src, u8 operand_size);
u64 EmulateLzcnt(u64 src, u8 operand_size);
u64 EmulateRorx(u64 src, u8 shift, u8 operand_size);
u64 EmulateSarx(u64 src, u64 shift, u8 operand_size);
u64 EmulateShlx(u64 src, u64 shift, u8 operand_size);
u64 EmulateShrx(u64 src, u64 shift, u8 operand_size);
u64 EmulatePdep(u64 src, u64 mask, u8 operand_size);
u64 EmulatePext(u64 src, u64 mask, u8 operand_size);

} // namespace CpuCore::AmdCompat
