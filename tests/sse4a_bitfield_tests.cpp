// SSE4a bit-field emulator tests (port of SharpEmu
// tests/SharpEmu.Libs.Tests/Cpu/Sse4aBitFieldEmulatorTests.cs, commit 8ef5a54).
//
// Exercises the pure EXTRQ/INSERTQ bit-field semantics used by the AMD-only
// illegal-instruction software fallback (src/cpu/amd_compat.cpp), plus the
// hand-rolled byte decoder that replaces upstream's Iced-based decoding.
// Expected values follow the AMD64 Architecture Programmer's Manual
// definitions of the immediate-form EXTRQ/INSERTQ semantics.
//
// Self-contained: no Memory / Loader / HLE dependencies.

#include "cpu/amd_compat.h"

#include <cstdio>

namespace {

using CpuCore::AmdCompat::Decode;
using CpuCore::AmdCompat::DecodedInstruction;
using CpuCore::AmdCompat::ExtractBitField;
using CpuCore::AmdCompat::InsertBitField;
using CpuCore::AmdCompat::InstructionKind;
using CpuCore::AmdCompat::IsValidBitField;

int g_failures = 0;
int g_checks   = 0;

#define EXPECT(cond, msg) do {                                     \
    ++g_checks;                                                    \
    if (!(cond)) {                                                 \
        ++g_failures;                                              \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n",                  \
                     __FILE__, __LINE__, (msg));                   \
    }                                                              \
} while (0)

#define EXPECT_EQ64(a, b, msg) do {                                \
    ++g_checks;                                                    \
    const u64 _av = (a);                                           \
    const u64 _bv = (b);                                           \
    if (_av != _bv) {                                              \
        ++g_failures;                                              \
        std::fprintf(stderr,                                       \
            "FAIL [%s:%d] %s: got 0x%llx, expected 0x%llx\n",      \
            __FILE__, __LINE__, (msg),                             \
            (unsigned long long)_av, (unsigned long long)_bv);     \
    }                                                              \
} while (0)

// --- Bit-field semantics (ported from the upstream test suite) -------------

void TestExtractLowByte() {
    EXPECT_EQ64(ExtractBitField(0x123456789ABCDEF0ULL, 8, 0), 0xF0ULL,
                "ExtractBitField extracts low byte");
}

void TestExtractMidField() {
    // bits [31:16] of 0x1234_5678_9ABC_DEF0 == 0x9ABC
    EXPECT_EQ64(ExtractBitField(0x123456789ABCDEF0ULL, 16, 16), 0x9ABCULL,
                "ExtractBitField extracts mid field at non-zero index");
}

void TestExtractLengthZeroMeansSixtyFour() {
    EXPECT_EQ64(ExtractBitField(0xFFFFFFFFFFFFFFFFULL, 0, 0), 0xFFFFFFFFFFFFFFFFULL,
                "ExtractBitField length zero means 64");
}

void TestExtractMasksImmediatesToLowSixBits() {
    // length=0x28 (40), index=0 — must agree with any narrower idiom handling.
    EXPECT_EQ64(ExtractBitField(0x00000000000000FFULL, 0x28, 0), 0xFFULL,
                "ExtractBitField masks immediates to low six bits");
}

void TestExtractByteFourRule() {
    // After "EXTRQ xmmN, 0x28, 0x00", dword lane 1 (bits 63:32) of the result
    // equals byte 4 of the source zero-extended.
    const u64 values[] = {
        0x123456789ABCDEF0ULL, 0x0000000000000000ULL,
        0xFFFFFFFFFFFFFFFFULL, 0x00FF00FF00FF00FFULL,
    };
    for (u64 value : values) {
        const u64 extracted = ExtractBitField(value, 0x28, 0);
        const u32 dword1 = static_cast<u32>(extracted >> 32);
        const u32 byte4 = static_cast<u32>((value >> 32) & 0xFF);
        EXPECT_EQ64(dword1, byte4, "ExtractBitField agrees with byte-4 rule");
    }
}

void TestExtractRejectsFieldPastRegisterEnd() {
    EXPECT(!IsValidBitField(8, 60), "IsValidBitField rejects field past bit 63");
    EXPECT_EQ64(ExtractBitField(0xFFFFFFFFFFFFFFFFULL, 8, 60), 0ULL,
                "ExtractBitField returns 0 for undefined field");
}

void TestExtractRejectsZeroLengthAtNonZeroIndex() {
    EXPECT(!IsValidBitField(0, 1), "IsValidBitField rejects len=0 at non-zero index");
}

void TestInsertAtNonZeroIndex() {
    EXPECT_EQ64(InsertBitField(0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL, 8, 8),
                0x000000000000FF00ULL,
                "InsertBitField inserts field without disturbing other bits");
}

void TestInsertClearsExactlyDestinationWindow() {
    EXPECT_EQ64(InsertBitField(0xFFFFFFFFFFFFFFFFULL, 0x0000000000000000ULL, 16, 16),
                0xFFFFFFFF0000FFFFULL,
                "InsertBitField clears exactly the destination window");
}

void TestInsertLowByteAtIndexZero() {
    EXPECT_EQ64(InsertBitField(0x1122334455667788ULL, 0xAABBCCDDEEFF0011ULL, 8, 0),
                0x1122334455667711ULL,
                "InsertBitField inserts low byte at index zero");
}

void TestInsertLengthZeroMeansSixtyFour() {
    EXPECT_EQ64(InsertBitField(0x1111111111111111ULL, 0xFFFFFFFFFFFFFFFFULL, 0, 0),
                0xFFFFFFFFFFFFFFFFULL,
                "InsertBitField length zero overwrites everything");
}

void TestInsertZeroFieldClearsOnlyItsWindow() {
    // A zero-valued 12-bit field at index 20 clears exactly bits [31:20]
    // (0x234 -> 0x000) and leaves every other bit untouched.
    EXPECT_EQ64(InsertBitField(0xABCDEF0123456789ULL, 0, 12, 20),
                0xABCDEF0100056789ULL,
                "InsertBitField zero source clears only its own window");
}

// --- Decoder coverage (replaces upstream's Iced-based decode) ---------------

void TestDecodeExtrqImmediate() {
    // 66 0F 78 C1 28 00  ==  EXTRQ xmm1, 0x28, 0x00
    const u8 code[] = {0x66, 0x0F, 0x78, 0xC1, 0x28, 0x00};
    DecodedInstruction insn{};
    EXPECT(Decode(code, sizeof(code), &insn), "decodes EXTRQ immediate form");
    EXPECT(insn.kind == InstructionKind::Extrq, "EXTRQ kind");
    EXPECT(insn.length == 6, "EXTRQ length 6");
    EXPECT(insn.dest_xmm == 1, "EXTRQ dest xmm1");
    EXPECT(insn.field_len == 0x28 && insn.field_idx == 0x00, "EXTRQ immediates");
}

void TestDecodeExtrqWithRexB() {
    // 66 41 0F 78 C0 10 04  ==  EXTRQ xmm8, 0x10, 0x04 (REX.B extends rm)
    const u8 code[] = {0x66, 0x41, 0x0F, 0x78, 0xC0, 0x10, 0x04};
    DecodedInstruction insn{};
    EXPECT(Decode(code, sizeof(code), &insn), "decodes EXTRQ with REX.B");
    EXPECT(insn.kind == InstructionKind::Extrq, "REX EXTRQ kind");
    EXPECT(insn.length == 7, "REX EXTRQ length 7");
    EXPECT(insn.dest_xmm == 8, "REX EXTRQ dest xmm8");
    EXPECT(insn.field_len == 0x10 && insn.field_idx == 0x04, "REX EXTRQ immediates");
}

void TestDecodeInsertqImmediate() {
    // F2 0F 78 D1 08 02  ==  INSERTQ xmm1, xmm2, 0x08, 0x02
    const u8 code[] = {0xF2, 0x0F, 0x78, 0xD1, 0x08, 0x02};
    DecodedInstruction insn{};
    EXPECT(Decode(code, sizeof(code), &insn), "decodes INSERTQ immediate form");
    EXPECT(insn.kind == InstructionKind::Insertq, "INSERTQ kind");
    EXPECT(insn.length == 6, "INSERTQ length 6");
    EXPECT(insn.dest_xmm == 1 && insn.src_xmm == 2, "INSERTQ dest/src");
    EXPECT(insn.field_len == 0x08 && insn.field_idx == 0x02, "INSERTQ immediates");
}

void TestDecodeInsertqWithRex() {
    // F2 45 0F 78 C8 40 00  ==  INSERTQ xmm8, xmm9, 0x40, 0x00 (REX.R + REX.B)
    const u8 code[] = {0xF2, 0x45, 0x0F, 0x78, 0xC8, 0x40, 0x00};
    DecodedInstruction insn{};
    EXPECT(Decode(code, sizeof(code), &insn), "decodes INSERTQ with REX");
    EXPECT(insn.kind == InstructionKind::Insertq, "REX INSERTQ kind");
    EXPECT(insn.length == 7, "REX INSERTQ length 7");
    EXPECT(insn.dest_xmm == 8 && insn.src_xmm == 9, "REX INSERTQ dest/src");
}

void TestDecodeMonitorxMwaitx() {
    const u8 monitorx[] = {0x0F, 0x01, 0xFA};
    const u8 mwaitx[]   = {0x0F, 0x01, 0xFB};
    DecodedInstruction insn{};
    EXPECT(Decode(monitorx, sizeof(monitorx), &insn) &&
           insn.kind == InstructionKind::Monitorx && insn.length == 3,
           "decodes MONITORX");
    EXPECT(Decode(mwaitx, sizeof(mwaitx), &insn) &&
           insn.kind == InstructionKind::Mwaitx && insn.length == 3,
           "decodes MWAITX");
}

void TestDecodeRejectsNonRecoverable() {
    DecodedInstruction insn{};
    // EXTRQ with a non-zero ModRM reg field (not the /0 encoding).
    const u8 bad_extrq[] = {0x66, 0x0F, 0x78, 0xC9, 0x28, 0x00};
    EXPECT(!Decode(bad_extrq, sizeof(bad_extrq), &insn),
           "rejects EXTRQ with reg != 0");
    // Memory form (mod != 3).
    const u8 mem_form[] = {0x66, 0x0F, 0x78, 0x01, 0x28, 0x00};
    EXPECT(!Decode(mem_form, sizeof(mem_form), &insn), "rejects memory form");
    // Register-form EXTRQ (0F 79) — not covered, matching upstream.
    const u8 reg_form[] = {0x66, 0x0F, 0x79, 0xC1};
    EXPECT(!Decode(reg_form, sizeof(reg_form), &insn), "rejects 0F 79 register form");
    // An ordinary instruction (MOV eax, ebx).
    const u8 mov[] = {0x89, 0xD8, 0x90, 0x90};
    EXPECT(!Decode(mov, sizeof(mov), &insn), "rejects ordinary instruction");
    // Truncated EXTRQ.
    const u8 truncated[] = {0x66, 0x0F, 0x78};
    EXPECT(!Decode(truncated, sizeof(truncated), &insn), "rejects truncated input");
}

void TestBmiOperations() {
    using namespace CpuCore::AmdCompat;
    // ANDN: (~src1) & src2
    EXPECT_EQ64(EmulateAndn(0x0F0F0F0F0F0F0F0FULL, 0xFFFFFFFFFFFFFFFFULL, 64), 0xF0F0F0F0F0F0F0F0ULL, "ANDN 64-bit");
    EXPECT_EQ64(EmulateAndn(0x000000FF, 0x0000FFFF, 32), 0x0000FF00ULL, "ANDN 32-bit");

    // BLSI: (-src) & src
    EXPECT_EQ64(EmulateBlsi(0x00000010, 32), 0x10ULL, "BLSI low bit set");
    EXPECT_EQ64(EmulateBlsi(0x00000000, 32), 0x0ULL, "BLSI zero");

    // BLSMSK: (src - 1) ^ src
    EXPECT_EQ64(EmulateBlsmsk(0x00000008, 32), 0x0FULL, "BLSMSK");

    // BLSR: (src - 1) & src
    EXPECT_EQ64(EmulateBlsr(0x0000000C, 32), 0x08ULL, "BLSR");

    // BEXTR: extract bit field by start/length
    EXPECT_EQ64(EmulateBextr(0x123456789ABCDEF0ULL, (16 | (16 << 8)), 64), 0x9ABCULL, "BEXTR 64-bit");

    // BZHI: zero high bits starting at index
    EXPECT_EQ64(EmulateBzhi(0xFFFFFFFFFFFFFFFFULL, 16, 64), 0xFFFFULL, "BZHI 16");

    // TZCNT / LZCNT
    EXPECT_EQ64(EmulateTzcnt(0x00000008, 32), 3ULL, "TZCNT");
    EXPECT_EQ64(EmulateTzcnt(0, 32), 32ULL, "TZCNT zero 32-bit");
    EXPECT_EQ64(EmulateLzcnt(0x00000008, 32), 28ULL, "LZCNT");
    EXPECT_EQ64(EmulateLzcnt(0, 32), 32ULL, "LZCNT zero 32-bit");

    // RORX / SARX / SHLX / SHRX
    EXPECT_EQ64(EmulateRorx(0x10000000, 4, 32), 0x01000000ULL, "RORX");
    EXPECT_EQ64(EmulateSarx(0x80000000, 4, 32), 0xF8000000ULL, "SARX");
    EXPECT_EQ64(EmulateShlx(0x00000001, 8, 32), 0x00000100ULL, "SHLX");
    EXPECT_EQ64(EmulateShrx(0x00000100, 8, 32), 0x00000001ULL, "SHRX");

    // PDEP / PEXT
    EXPECT_EQ64(EmulatePdep(0x55, 0x00FF, 32), 0x55ULL, "PDEP low byte");
    EXPECT_EQ64(EmulatePdep(0x55, 0xFF00, 32), 0x5500ULL, "PDEP mid byte");
    EXPECT_EQ64(EmulatePext(0x12345678, 0x0F0F0F0F, 32), 0x2468ULL, "PEXT");
}

} // namespace

int main() {
    TestExtractLowByte();
    TestExtractMidField();
    TestExtractLengthZeroMeansSixtyFour();
    TestExtractMasksImmediatesToLowSixBits();
    TestExtractByteFourRule();
    TestExtractRejectsFieldPastRegisterEnd();
    TestExtractRejectsZeroLengthAtNonZeroIndex();
    TestInsertAtNonZeroIndex();
    TestInsertClearsExactlyDestinationWindow();
    TestInsertLowByteAtIndexZero();
    TestInsertLengthZeroMeansSixtyFour();
    TestInsertZeroFieldClearsOnlyItsWindow();
    TestDecodeExtrqImmediate();
    TestDecodeExtrqWithRexB();
    TestDecodeInsertqImmediate();
    TestDecodeInsertqWithRex();
    TestDecodeMonitorxMwaitx();
    TestDecodeRejectsNonRecoverable();
    TestBmiOperations();

    std::printf("sse4a_bitfield tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
