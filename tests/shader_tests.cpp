// Phase 5 M2 shader stack tests:
//  - GCN/RDNA2 decoder: one hand-built instruction per encoding family,
//    literal-constant sizing, full-program round-trip.
//  - AGC metadata: synthetic shader header + synthetic shader ELF.
//  - SPIR-V module builder: golden word stream, id/dedup invariants.

#include "gpu/shader/gcn_decode.h"
#include "gpu/shader/gcn_translate.h"
#include "gpu/shader/metadata.h"
#include "gpu/shader/spirv_builder.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks   = 0;

#define EXPECT(cond, msg)                                                            \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg);    \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

#define EXPECT_EQ(a, b, msg)                                                        \
    do {                                                                            \
        ++g_checks;                                                                 \
        auto _lhs = (a);                                                            \
        auto _rhs = (b);                                                            \
        if (!(_lhs == _rhs)) {                                                      \
            std::fprintf(stderr, "[FAIL] %s:%d: %s  (lhs=%lld rhs=%lld)\n",         \
                         __FILE__, __LINE__, msg,                                   \
                         (long long)_lhs, (long long)_rhs);                         \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

#define EXPECT_STR_EQ(a, b, msg)                                                    \
    do {                                                                            \
        ++g_checks;                                                                 \
        std::string _lhs = (a);                                                     \
        std::string _rhs = (b);                                                     \
        if (_lhs != _rhs) {                                                         \
            std::fprintf(stderr, "[FAIL] %s:%d: %s  (lhs=\"%s\" rhs=\"%s\")\n",     \
                         __FILE__, __LINE__, msg, _lhs.c_str(), _rhs.c_str());      \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

using namespace GPU::Shader;

// Decodes a single instruction from a dword array and returns it.
bool DecodeOne(const std::vector<u32>& dwords, GcnInstruction& out) {
    u32         size = 0;
    std::string error;
    if (!GcnDecodeInstruction(
            dwords.data(), dwords.size(), 0, out, size, error)) {
        std::fprintf(stderr, "decode error: %s\n", error.c_str());
        return false;
    }
    EXPECT_EQ(out.words.size(), static_cast<size_t>(size), "size vs words");
    return true;
}

// ---------------------------------------------------------------------------
// Decoder: one instruction per encoding family.
// ---------------------------------------------------------------------------

void TestSop2() {
    // S_ADD_U32 s2, s0, s1
    GcnInstruction ins;
    EXPECT(DecodeOne({0x80020100}, ins), "sop2 decode");
    EXPECT(ins.encoding == GcnEncoding::Sop2, "sop2 encoding");
    EXPECT_STR_EQ(ins.opcode, "SAddU32", "sop2 name");
    EXPECT_EQ(ins.sources.size(), 2u, "sop2 srcs");
    EXPECT_EQ(ins.destinations.size(), 1u, "sop2 dsts");
    EXPECT_EQ(ins.destinations[0].value, 2u, "sop2 sdst");
    EXPECT(ins.sources[0].kind == GcnOperandKind::ScalarRegister, "sop2 src0 kind");
    EXPECT_EQ(ins.sources[1].value, 1u, "sop2 ssrc1");
}

void TestSop2Literal() {
    // S_ADD_U32 s2, 0xDEADBEEF, s1 — literal trailing dword.
    GcnInstruction ins;
    EXPECT(DecodeOne({0x800200FF, 0xDEADBEEF}, ins), "sop2 literal decode");
    EXPECT_EQ(ins.words.size(), 2u, "sop2 literal size");
    EXPECT(ins.sources[0].kind == GcnOperandKind::LiteralConstant,
           "sop2 literal kind");
    EXPECT_EQ(ins.sources[0].value, 0xDEADBEEFu, "sop2 literal value");
}

void TestSop1() {
    // S_MOV_B32 s5, s6
    GcnInstruction ins;
    EXPECT(DecodeOne({0xBE850306}, ins), "sop1 decode");
    EXPECT(ins.encoding == GcnEncoding::Sop1, "sop1 encoding");
    EXPECT_STR_EQ(ins.opcode, "SMovB32", "sop1 name");
    EXPECT_EQ(ins.destinations[0].value, 5u, "sop1 sdst");
    EXPECT_EQ(ins.sources[0].value, 6u, "sop1 ssrc0");
}

void TestSopc() {
    // S_CMP_EQ_I32 s1, s2
    GcnInstruction ins;
    EXPECT(DecodeOne({0xBF000201}, ins), "sopc decode");
    EXPECT(ins.encoding == GcnEncoding::Sopc, "sopc encoding");
    EXPECT_STR_EQ(ins.opcode, "SCmpEqI32", "sopc name");
    EXPECT_EQ(ins.sources.size(), 2u, "sopc srcs");
    EXPECT_EQ(ins.destinations.size(), 0u, "sopc dsts");
}

void TestSopp() {
    // S_BRANCH +5
    GcnInstruction ins;
    EXPECT(DecodeOne({0xBF820005}, ins), "sopp decode");
    EXPECT(ins.encoding == GcnEncoding::Sopp, "sopp encoding");
    EXPECT_STR_EQ(ins.opcode, "SBranch", "sopp name");
    // S_ENDPGM
    GcnInstruction end;
    EXPECT(DecodeOne({0xBF810000}, end), "sopp endpgm decode");
    EXPECT_STR_EQ(end.opcode, "SEndpgm", "sopp endpgm name");
}

void TestSopk() {
    // S_MOVK_I32 s3, 0x1234
    GcnInstruction ins;
    EXPECT(DecodeOne({0xB0031234}, ins), "sopk decode");
    EXPECT(ins.encoding == GcnEncoding::Sopk, "sopk encoding");
    EXPECT_STR_EQ(ins.opcode, "SMovkI32", "sopk name");
    EXPECT_EQ(ins.destinations[0].value, 3u, "sopk sdst");
    EXPECT(ins.sources[0].kind == GcnOperandKind::EncodedConstant,
           "sopk imm kind");
    EXPECT_EQ(ins.sources[0].value, 0x1234u, "sopk simm16");
}

void TestSmrd() {
    // S_LOAD_DWORD s4, s[6:7], 0x10 (immediate offset -> 0x40 bytes)
    GcnInstruction ins;
    EXPECT(DecodeOne({0xC0020710}, ins), "smrd decode");
    EXPECT(ins.encoding == GcnEncoding::Smrd, "smrd encoding");
    EXPECT_STR_EQ(ins.opcode, "SLoadDword", "smrd name");
    EXPECT_EQ(ins.sources[0].value, 6u, "smrd sbase");
    EXPECT_EQ(ins.destinations[0].value, 4u, "smrd sdst");
    const auto* control = std::get_if<GcnScalarMemoryControl>(&ins.control);
    EXPECT(control != nullptr, "smrd control");
    if (control) {
        EXPECT_EQ(control->immediate_offset_bytes, 0x40, "smrd offset bytes");
        EXPECT_EQ(control->destination_count, 1u, "smrd count");
    }
}

void TestSmem() {
    // S_BUFFER_LOAD_DWORDX4 s[8:11], s[6:7], s16 offset 0x40
    GcnInstruction ins;
    EXPECT(DecodeOne({0xF4280203, 0x20000040}, ins), "smem decode");
    EXPECT(ins.encoding == GcnEncoding::Smem, "smem encoding");
    EXPECT_STR_EQ(ins.opcode, "SBufferLoadDwordx4", "smem name");
    EXPECT_EQ(ins.destinations.size(), 4u, "smem dst count");
    EXPECT_EQ(ins.destinations[0].value, 8u, "smem sdst");
    const auto* control = std::get_if<GcnScalarMemoryControl>(&ins.control);
    EXPECT(control != nullptr, "smem control");
    if (control) {
        EXPECT_EQ(control->immediate_offset_bytes, 0x40, "smem offset");
        EXPECT(control->dynamic_offset_register.has_value(),
               "smem dynamic offset");
        if (control->dynamic_offset_register) {
            EXPECT_EQ(*control->dynamic_offset_register, 16u, "smem soffset");
        }
    }
}

void TestVop1() {
    // V_MOV_B32 v1, v2
    GcnInstruction ins;
    EXPECT(DecodeOne({0x7E020302}, ins), "vop1 decode");
    EXPECT(ins.encoding == GcnEncoding::Vop1, "vop1 encoding");
    EXPECT_STR_EQ(ins.opcode, "VMovB32", "vop1 name");
    EXPECT(ins.sources[0].kind == GcnOperandKind::VectorRegister, "vop1 src kind");
    EXPECT_EQ(ins.sources[0].value, 2u, "vop1 src");
    EXPECT_EQ(ins.destinations[0].value, 1u, "vop1 vdst");
}

void TestVop1Readfirstlane() {
    // V_READFIRSTLANE_B32 s7, v9 — scalar destination special case.
    const u32 word = 0x7E000000 | (7u << 17) | (0x02u << 9) | (256u + 9u);
    GcnInstruction ins;
    EXPECT(DecodeOne({word}, ins), "readfirstlane decode");
    EXPECT_STR_EQ(ins.opcode, "VReadfirstlaneB32", "readfirstlane name");
    EXPECT(ins.destinations[0].kind == GcnOperandKind::ScalarRegister,
           "readfirstlane dst kind");
    EXPECT_EQ(ins.destinations[0].value, 7u, "readfirstlane sdst");
}

void TestVop2() {
    // V_ADD_F32 v1, v2, v3
    GcnInstruction ins;
    EXPECT(DecodeOne({0x06020702}, ins), "vop2 decode");
    EXPECT(ins.encoding == GcnEncoding::Vop2, "vop2 encoding");
    EXPECT_STR_EQ(ins.opcode, "VAddF32", "vop2 name");
    EXPECT_EQ(ins.sources[0].value, 2u, "vop2 src0");
    EXPECT_EQ(ins.sources[1].value, 3u, "vop2 vsrc1");
    EXPECT_EQ(ins.destinations[0].value, 1u, "vop2 vdst");
}

void TestVopc() {
    // V_CMP_LT_F32 v1, v2
    GcnInstruction ins;
    EXPECT(DecodeOne({0x7C020501}, ins), "vopc decode");
    EXPECT(ins.encoding == GcnEncoding::Vopc, "vopc encoding");
    EXPECT_STR_EQ(ins.opcode, "VCmpLtF32", "vopc name");
    EXPECT_EQ(ins.sources[0].value, 1u, "vopc src0");
    EXPECT_EQ(ins.sources[1].value, 2u, "vopc vsrc1");
}

void TestVop3() {
    // V_FMA_F32 v5, v1, v2, v3
    GcnInstruction ins;
    EXPECT(DecodeOne({0xD54B0005, 0x040E0501}, ins), "vop3 decode");
    EXPECT(ins.encoding == GcnEncoding::Vop3, "vop3 encoding");
    EXPECT_STR_EQ(ins.opcode, "VFmaF32", "vop3 name");
    EXPECT_EQ(ins.sources.size(), 3u, "vop3 srcs");
    EXPECT_EQ(ins.sources[2].value, 3u, "vop3 src2");
    EXPECT_EQ(ins.destinations[0].value, 5u, "vop3 vdst");
    EXPECT(std::get_if<GcnVop3Control>(&ins.control) != nullptr,
           "vop3 control");
}

void TestVop3Literal() {
    // V_ADD_F32 v5, 0x3F800000, v2 — literal in the VOP3 src0 slot forces a
    // third dword (regression: the literal indicator is 0xFF, not 0x1FF).
    GcnInstruction ins;
    EXPECT(DecodeOne({0xD5030005, 0x000204FF, 0x3F800000}, ins),
           "vop3 literal decode");
    EXPECT_EQ(ins.words.size(), 3u, "vop3 literal size");
    EXPECT_STR_EQ(ins.opcode, "VAddF32", "vop3 literal name");
    EXPECT(ins.sources[0].kind == GcnOperandKind::LiteralConstant,
           "vop3 literal kind");
    EXPECT_EQ(ins.sources[0].value, 0x3F800000u, "vop3 literal value");
    EXPECT_EQ(ins.sources[1].value, 2u, "vop3 literal src1");
}

void TestVop3p() {
    // V_PK_FMA_F16 v5, v1, v2, v3
    GcnInstruction ins;
    EXPECT(DecodeOne({0xCC0E0005, 0x040E0501}, ins), "vop3p decode");
    EXPECT(ins.encoding == GcnEncoding::Vop3p, "vop3p encoding");
    EXPECT_STR_EQ(ins.opcode, "VPkFmaF16", "vop3p name");
    EXPECT_EQ(ins.destinations[0].value, 5u, "vop3p vdst");
    EXPECT(std::get_if<GcnVop3pControl>(&ins.control) != nullptr,
           "vop3p control");
}

void TestVintrp() {
    // V_INTERP_P1_F32 v1, v2, attr0.x
    GcnInstruction ins;
    EXPECT(DecodeOne({0xC8040002}, ins), "vintrp decode");
    EXPECT(ins.encoding == GcnEncoding::Vintrp, "vintrp encoding");
    EXPECT_STR_EQ(ins.opcode, "VInterpP1F32", "vintrp name");
    EXPECT_EQ(ins.destinations[0].value, 1u, "vintrp vdst");
    const auto* control = std::get_if<GcnInterpolationControl>(&ins.control);
    EXPECT(control != nullptr, "vintrp control");
}

void TestDs() {
    // DS_WRITE_B32 v1, v2, offset0:0x10 offset1:2
    GcnInstruction ins;
    EXPECT(DecodeOne({0xD8340210, 0x00000201}, ins), "ds decode");
    EXPECT(ins.encoding == GcnEncoding::Ds, "ds encoding");
    EXPECT_STR_EQ(ins.opcode, "DsWriteB32", "ds name");
    EXPECT_EQ(ins.sources.size(), 2u, "ds srcs");
    const auto* control = std::get_if<GcnDataShareControl>(&ins.control);
    EXPECT(control != nullptr, "ds control");
    if (control) {
        EXPECT_EQ(control->offset0, 0x10u, "ds offset0");
        EXPECT_EQ(control->offset1, 2u, "ds offset1");
    }
}

void TestFlat() {
    // GLOBAL_LOAD_DWORD v2, v1, s3, offset 0x20
    GcnInstruction ins;
    EXPECT(DecodeOne({0xDC308020, 0x00030201}, ins), "flat decode");
    EXPECT(ins.encoding == GcnEncoding::Flat, "flat encoding");
    EXPECT_STR_EQ(ins.opcode, "GlobalLoadDword", "flat name");
    EXPECT_EQ(ins.destinations.size(), 1u, "flat dsts");
    EXPECT_EQ(ins.destinations[0].value, 2u, "flat vdata");
    const auto* control = std::get_if<GcnGlobalMemoryControl>(&ins.control);
    EXPECT(control != nullptr, "flat control");
    if (control) {
        EXPECT_EQ(control->offset_bytes, 0x20, "flat offset");
        EXPECT_EQ(control->dword_count, 1u, "flat count");
    }
}

void TestMubuf() {
    // BUFFER_LOAD_FORMAT_XYZW v[2:5], v1, s[12:15], s16 idxen+offen offset 0x10
    GcnInstruction ins;
    EXPECT(DecodeOne({0xE00C3010, 0x10030201}, ins), "mubuf decode");
    EXPECT(ins.encoding == GcnEncoding::Mubuf, "mubuf encoding");
    EXPECT_STR_EQ(ins.opcode, "BufferLoadFormatXyzw", "mubuf name");
    EXPECT_EQ(ins.destinations.size(), 4u, "mubuf dsts");
    const auto* control = std::get_if<GcnBufferMemoryControl>(&ins.control);
    EXPECT(control != nullptr, "mubuf control");
    if (control) {
        EXPECT_EQ(control->scalar_resource, 12u, "mubuf srsrc");
        EXPECT(control->index_enabled && control->offset_enabled,
               "mubuf idxen/offen");
        EXPECT_EQ(control->offset_bytes, 0x10, "mubuf offset");
    }
}

void TestMtbuf() {
    // TBUFFER_LOAD_FORMAT_X v2, v1, s[12:15], s16, offset 0x10
    GcnInstruction ins;
    EXPECT(DecodeOne({0xE8000010, 0x10030201}, ins), "mtbuf decode");
    EXPECT(ins.encoding == GcnEncoding::Mtbuf, "mtbuf encoding");
    EXPECT_STR_EQ(ins.opcode, "TBufferLoadFormatX", "mtbuf name");
    EXPECT_EQ(ins.destinations.size(), 1u, "mtbuf dsts");
}

void TestMimg() {
    // IMAGE_SAMPLE v6, v5, s[8:11], s[12:15], dmask 0xF, 2D
    GcnInstruction ins;
    EXPECT(DecodeOne({0xF0800F08, 0x00620605}, ins), "mimg decode");
    EXPECT(ins.encoding == GcnEncoding::Mimg, "mimg encoding");
    EXPECT_STR_EQ(ins.opcode, "ImageSample", "mimg name");
    const auto* control = std::get_if<GcnImageControl>(&ins.control);
    EXPECT(control != nullptr, "mimg control");
    if (control) {
        EXPECT_EQ(control->dmask, 0xFu, "mimg dmask");
        EXPECT_EQ(control->dimension, 1u, "mimg dim");
        EXPECT_EQ(control->scalar_resource, 8u, "mimg srsrc");
        EXPECT_EQ(control->scalar_sampler, 12u, "mimg ssamp");
        EXPECT(!control->is_array, "mimg array");
    }
    EXPECT_EQ(ins.destinations[0].value, 6u, "mimg vdata");
}

void TestExp() {
    // EXP en=0xF target 0, done, v1..v4
    GcnInstruction ins;
    EXPECT(DecodeOne({0xF800080F, 0x04030201}, ins), "exp decode");
    EXPECT(ins.encoding == GcnEncoding::Exp, "exp encoding");
    EXPECT_STR_EQ(ins.opcode, "Exp", "exp name");
    EXPECT_EQ(ins.sources.size(), 4u, "exp srcs");
    const auto* control = std::get_if<GcnExportControl>(&ins.control);
    EXPECT(control != nullptr, "exp control");
    if (control) {
        EXPECT_EQ(control->enable_mask, 0xFu, "exp en");
        EXPECT(control->done, "exp done");
        EXPECT(!control->compressed, "exp compr");
    }
}

void TestUnknownOpcodeDecodes() {
    // SOP2 opcode 0x40 (reserved) must still decode structurally.
    GcnInstruction ins;
    const u32 word = 0x80000000 | (0x40u << 23) | (2u << 16) | 1u;
    EXPECT(DecodeOne({word}, ins), "unknown sop2 decodes");
    EXPECT(ins.encoding == GcnEncoding::Sop2, "unknown encoding");
    EXPECT(!ins.opcode_known, "unknown flagged");
    EXPECT(ins.opcode.find("Sop2Raw") == 0, "unknown name");
}

void TestProgramRoundTrip() {
    const std::vector<u32> code = {
        0xBE850306, // S_MOV_B32 s5, s6
        0x06020702, // V_ADD_F32 v1, v2, v3
        0xBF810000, // S_ENDPGM
        0x00000000, // padding
        0x00000000,
    };
    GcnProgram  program;
    std::string error;
    EXPECT(GcnDecodeProgram(code.data(), code.size(), program, error),
           "program decode");
    if (!error.empty()) {
        std::fprintf(stderr, "program error: %s\n", error.c_str());
    }
    EXPECT_EQ(program.instructions.size(), 3u, "program count");
    EXPECT_STR_EQ(program.instructions[1].opcode, "VAddF32", "program ins1");
    EXPECT_EQ(program.instructions[2].pc, 8u, "program pc");
}

// ---------------------------------------------------------------------------
// Metadata: synthetic shader header.
// ---------------------------------------------------------------------------

void WriteU16(std::vector<u8>& buffer, size_t offset, u16 value) {
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
}
void WriteU32(std::vector<u8>& buffer, size_t offset, u32 value) {
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
}
void WriteU64(std::vector<u8>& buffer, size_t offset, u64 value) {
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

std::vector<u8> BuildSyntheticHeader() {
    std::vector<u8> header(0x160, 0);
    WriteU32(header, 0x00, kAgcShaderFileMagic);
    WriteU32(header, 0x04, kAgcShaderFileVersion);
    // userData block at 0x100 (self-relative from field at 0x08).
    WriteU64(header, 0x08, 0x100 - 0x08);
    // SH register table at 0x60, two entries.
    WriteU64(header, 0x20, 0x60 - 0x20);
    WriteU32(header, 0x60, 0x2C12);
    WriteU32(header, 0x64, 0xCAFE);
    WriteU32(header, 0x68, 0x2C13);
    WriteU32(header, 0x6C, 0xBABE);
    // input semantics (2) at 0x80, output semantics (1) at 0x88.
    WriteU64(header, 0x30, 0x80 - 0x30);
    WriteU64(header, 0x38, 0x88 - 0x38);
    WriteU32(header, 0x80, 0x30900);
    WriteU32(header, 0x84, 0x20C02);
    WriteU32(header, 0x88, 0x0F);
    WriteU16(header, 0x50, 2); // num input semantics
    WriteU16(header, 0x56, 1); // num output semantics
    header[0x5A] = AgcShaderType::Pixel;
    header[0x5C] = 2; // num SH registers

    // userData at 0x100: direct-resource offsets table at 0x140.
    WriteU64(header, 0x100, 0x140 - 0x100);
    // resource class tables: samplers (class 2) at 0x150, CBs (class 3) 0x154.
    WriteU64(header, 0x118, 0x150 - 0x118); // class 2 field
    WriteU64(header, 0x120, 0x154 - 0x120); // class 3 field
    WriteU16(header, 0x128, 0x20); // extended user data size
    WriteU16(header, 0x12A, 0);    // shader resource table size
    WriteU16(header, 0x12C, 3);    // direct resource count
    WriteU16(header, 0x12E, 0);    // class 0 count
    WriteU16(header, 0x130, 0);    // class 1 count
    WriteU16(header, 0x132, 2);    // class 2 count
    WriteU16(header, 0x134, 1);    // class 3 count
    WriteU16(header, 0x140, 0xFFFF);
    WriteU16(header, 0x142, 0x0004);
    WriteU16(header, 0x144, 0x0006);
    WriteU16(header, 0x150, 0x8010);
    WriteU16(header, 0x152, 0x8014);
    WriteU16(header, 0x154, 0x0018);
    return header;
}

void TestMetadataHeader() {
    const std::vector<u8> header = BuildSyntheticHeader();
    AgcShaderHeader parsed;
    std::string     error;
    EXPECT(AgcParseHeader(header, parsed, error), "header parse");
    if (!error.empty()) {
        std::fprintf(stderr, "header error: %s\n", error.c_str());
    }
    EXPECT_EQ(parsed.shader_type, AgcShaderType::Pixel, "shader type");
    EXPECT_EQ(parsed.num_sh_registers, 2u, "sh count");
    EXPECT_EQ(parsed.sh_registers.size(), 2u, "sh entries");
    if (parsed.sh_registers.size() == 2) {
        EXPECT_EQ(parsed.sh_registers[0].reg, 0x2C12u, "sh reg0");
        EXPECT_EQ(parsed.sh_registers[1].value, 0xBABEu, "sh val1");
    }
    EXPECT_EQ(parsed.input_semantics.size(), 2u, "in sem count");
    if (parsed.input_semantics.size() == 2) {
        EXPECT_EQ(parsed.input_semantics[0], 0x30900u, "in sem0");
        EXPECT_EQ(parsed.input_semantics[1], 0x20C02u, "in sem1");
    }
    EXPECT_EQ(parsed.output_semantics.size(), 1u, "out sem count");
    EXPECT(parsed.has_user_data, "has user data");
    EXPECT_EQ(parsed.user_data.extended_user_data_size_dwords, 0x20u, "ext ud");
    EXPECT_EQ(parsed.user_data.direct_resources.size(), 2u, "direct count");
    EXPECT_EQ(parsed.user_data.direct_resources.count(1u), 1u, "direct 1");
    EXPECT_EQ(parsed.user_data.direct_resources.count(2u), 1u, "direct 2");
    EXPECT_EQ(parsed.user_data.resources.size(), 3u, "resource count");
    size_t samplers = 0, cbs = 0;
    for (const auto& resource : parsed.user_data.resources) {
        if (resource.kind == AgcResourceKind::Sampler) {
            ++samplers;
            EXPECT(resource.size_flag, "sampler size flag");
        } else if (resource.kind == AgcResourceKind::ConstantBuffer) {
            ++cbs;
            EXPECT(!resource.size_flag, "cb size flag");
            EXPECT_EQ(resource.offset_dwords, 0x18u, "cb offset");
        }
    }
    EXPECT_EQ(samplers, 2u, "samplers");
    EXPECT_EQ(cbs, 1u, "cbs");
}

void TestMetadataElf() {
    // Minimal shader ELF64: null + .shader_text + .shader_header + .shstrtab.
    const std::vector<u8> header = BuildSyntheticHeader();
    const u32 code[] = {0xBE850306, 0xBF810000}; // S_MOV_B32 + S_ENDPGM

    const char shstrtab[] = "\0.shader_text\0.shader_header\0.shstrtab\0";
    constexpr u32 name_text   = 1;
    constexpr u32 name_header = 14;  // 1 + strlen(".shader_text") + 1
    constexpr u32 name_str    = 29;  // 14 + strlen(".shader_header") + 1

    std::vector<u8> elf(0x500, 0);
    elf[0] = 0x7F;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 2; // 64-bit
    elf[5] = 1; // LE
    elf[6] = 1; // version
    WriteU16(elf, 0x12, 2);      // e_type EXEC
    WriteU16(elf, 0x14, 0x00E0); // e_machine AMDGPU
    const u64 shoff = 0x200;
    WriteU64(elf, 0x28, shoff);
    WriteU16(elf, 0x34, 64); // ehsize
    WriteU16(elf, 0x3A, 64); // shentsize
    WriteU16(elf, 0x3C, 4);  // shnum
    WriteU16(elf, 0x3E, 3);  // shstrndx

    const u64 text_off   = 0x100;
    const u64 str_off    = 0x1C0;
    const u64 header_off = 0x300;
    std::memcpy(elf.data() + text_off, code, sizeof(code));
    std::memcpy(elf.data() + header_off, header.data(), header.size());
    std::memcpy(elf.data() + str_off, shstrtab, sizeof(shstrtab));

    const auto write_section = [&](u32 index, u32 name, u32 type, u64 offset,
                                   u64 size) {
        const u64 base = shoff + index * 64;
        WriteU32(elf, base, name);
        WriteU32(elf, base + 4, type);
        WriteU64(elf, base + 0x18, offset);
        WriteU64(elf, base + 0x20, size);
    };
    write_section(1, name_text, 1, text_off, sizeof(code));
    write_section(2, name_header, 1, header_off, header.size());
    write_section(3, name_str, 3, str_off, sizeof(shstrtab));

    AgcShaderFile file;
    std::string   error;
    EXPECT(AgcLoadShaderFile(elf, file, error), "elf load");
    if (!error.empty()) {
        std::fprintf(stderr, "elf error: %s\n", error.c_str());
    }
    EXPECT_EQ(file.code.size(), sizeof(code), "code span");
    EXPECT_EQ(file.header.size(), header.size(), "header span");
    EXPECT_EQ(file.parsed_header.shader_type, AgcShaderType::Pixel, "elf type");

    // The extracted code must decode end-to-end.
    GcnProgram  program;
    std::string decode_error;
    EXPECT(GcnDecodeProgram(
               reinterpret_cast<const u32*>(file.code.data()),
               file.code.size() / sizeof(u32), program, decode_error),
           "elf code decode");
    EXPECT_EQ(program.instructions.size(), 2u, "elf ins count");
}

// ---------------------------------------------------------------------------
// SPIR-V builder: golden words.
// ---------------------------------------------------------------------------

constexpr u32 InstWord(u32 word_count, u32 opcode) {
    return (word_count << 16) | opcode;
}

void TestSpirvGolden() {
    SpirvModuleBuilder builder;
    builder.AddCapability(SpirvCapability::Shader);
    const u32 void_type     = builder.TypeVoid();
    const u32 function_type = builder.TypeFunction(void_type, {});
    const u32 main_function = builder.BeginFunction(void_type, function_type);
    builder.AddEntryPoint(SpirvExecutionModel::Fragment, main_function, "main",
                          {});
    builder.AddExecutionMode(main_function, SpirvExecutionMode::OriginUpperLeft);
    const u32 label = builder.AddLabel();
    builder.AddStatement(SpirvOp::Return);
    builder.EndFunction();

    EXPECT_EQ(void_type, 1u, "void id");
    EXPECT_EQ(function_type, 2u, "fn type id");
    EXPECT_EQ(main_function, 3u, "main id");
    EXPECT_EQ(label, 4u, "label id");

    const std::vector<u32> words = builder.Build();
    const std::vector<u32> expected = {
        0x07230203, 0x00010500, 0x53504500, 5, 0, // header
        InstWord(2, 17), 1,                       // OpCapability Shader
        InstWord(3, 14), 0, 1,                    // OpMemoryModel Logical GLSL450
        InstWord(5, 15), 4, 3, 0x6E69616D, 0,     // OpEntryPoint Fragment %3 "main"
        InstWord(3, 16), 3, 7,                    // OpExecutionMode %3 OriginUpperLeft
        InstWord(2, 19), 1,                       // OpTypeVoid %1
        InstWord(3, 33), 2, 1,                    // OpTypeFunction %2 %1
        InstWord(5, 54), 1, 3, 0, 2,              // OpFunction %1 %3 None %2
        InstWord(2, 248), 4,                      // OpLabel %4
        InstWord(1, 253),                         // OpReturn
        InstWord(1, 56),                          // OpFunctionEnd
    };
    EXPECT_EQ(words.size(), expected.size(), "spirv word count");
    if (words.size() == expected.size()) {
        for (size_t i = 0; i < words.size(); ++i) {
            if (words[i] != expected[i]) {
                std::fprintf(stderr,
                             "[FAIL] spirv word[%zu]: got 0x%08X want 0x%08X\n",
                             i, words[i], expected[i]);
                ++g_failures;
                ++g_checks;
                break;
            }
        }
    }
}

void TestSpirvDedup() {
    SpirvModuleBuilder builder;
    const u32 int_a = builder.TypeInt(32, false);
    const u32 int_b = builder.TypeInt(32, false);
    const u32 int_s = builder.TypeInt(32, true);
    EXPECT_EQ(int_a, int_b, "int dedup");
    EXPECT(int_a != int_s, "int sign distinct");

    const u32 const_a = builder.Constant(int_a, 42);
    const u32 const_b = builder.Constant(int_a, 42);
    EXPECT_EQ(const_a, const_b, "const dedup");

    const u32 vec_a = builder.TypeVector(builder.TypeFloat(32), 4);
    const u32 vec_b = builder.TypeVector(builder.TypeFloat(32), 4);
    EXPECT_EQ(vec_a, vec_b, "vector dedup");

    const u32 ptr_a = builder.TypePointer(SpirvStorageClass::Function, int_a);
    const u32 ptr_b = builder.TypePointer(SpirvStorageClass::Function, int_a);
    EXPECT_EQ(ptr_a, ptr_b, "pointer dedup");

    const std::vector<u32> words = builder.Build();
    // bound must exceed every allocated id
    EXPECT(words[3] > ptr_a, "bound covers ids");
}

} // namespace

// ---------------------------------------------------------------------------
// Translation (Phase 5 M2 slice 2): decoded program -> SPIR-V module.
// ---------------------------------------------------------------------------
namespace {

// Walks a SPIR-V word stream looking for an instruction with `opcode`.
bool SpirvContainsOp(const std::vector<u32>& words, u16 opcode) {
    size_t cursor = 5; // header
    while (cursor < words.size()) {
        const u32 word = words[cursor];
        const u16 op = static_cast<u16>(word & 0xFFFF);
        const u32 count = word >> 16;
        if (count == 0) {
            return false; // malformed stream
        }
        if (op == opcode) {
            return true;
        }
        cursor += count;
    }
    return false;
}

// Finds an instruction carrying control type T and returns its pc.
template <typename T>
bool FindControlPc(const GcnProgram& program, u32& pc) {
    for (const GcnInstruction& ins : program.instructions) {
        if (std::get_if<T>(&ins.control)) {
            pc = ins.pc;
            return true;
        }
    }
    return false;
}

// Vertex: v_mov -> export -> endpgm.  Real encodings from the corpus.
void TestTranslateVertex() {
    const std::vector<u32> dwords = {
        0x7E1402F2,             // VMovB32 v10, src[242]
        0xF8000941, 0x00000000, // Exp v0, v0, v0, v0
        0xBF810000,             // SEndpgm
    };
    GcnProgram program;
    std::string error;
    EXPECT(GcnDecodeProgram(dwords.data(), dwords.size(), program, error),
           "vertex program decodes");

    GcnTranslateOptions options;
    options.stage = GcnSpirvStage::Vertex;
    GcnSpirvShader shader;
    EXPECT(GcnTranslateToSpirv(program, options, shader, error),
           "vertex program translates");
    EXPECT(shader.words.size() > 5, "spirv non-empty");
    EXPECT_EQ(shader.words[0], 0x07230203u, "spirv magic");
    // OpEntryPoint (15), OpDecorate (71), OpLoopMerge (246) dispatcher.
    EXPECT(SpirvContainsOp(shader.words, 15), "entry point present");
    EXPECT(SpirvContainsOp(shader.words, 71), "decorations present");
    EXPECT(SpirvContainsOp(shader.words, 246), "pc dispatcher loop present");
}

// Pixel: interp + image sample + arithmetic + export, corpus encodings.
void TestTranslatePixel() {
    const std::vector<u32> dwords = {
        0xC8100000,             // VInterpP1F32 v4, v0
        0xC8110001,             // VInterpP2F32 v4, v1
        0xF0800F08, 0x00400004, // ImageSample v0, v4, s0, s8
        0xBF8C0070,             // SWaitcnt
        0x10000010,             // VMulF32 v0, s16, v0
        0x5E000300,             // VCvtPkrtzF16F32 v0, v0, v1
        0xF8001C0F, 0x00000100, // Exp v0, v1, v0, v0 (compressed)
        0xBF810000,             // SEndpgm
    };
    GcnProgram program;
    std::string error;
    EXPECT(GcnDecodeProgram(dwords.data(), dwords.size(), program, error),
           "pixel program decodes");

    GcnTranslateOptions options;
    options.stage = GcnSpirvStage::Pixel;
    options.pixel_outputs.push_back(
        GcnPixelOutputBinding{0, 0, GcnPixelOutputKind::Float});
    u32 image_pc = 0;
    EXPECT(FindControlPc<GcnImageControl>(program, image_pc),
           "image instruction present");
    GcnSpirvImageBinding binding;
    binding.pc = image_pc;
    options.image_bindings.push_back(binding);

    GcnSpirvShader shader;
    EXPECT(GcnTranslateToSpirv(program, options, shader, error),
           "pixel program translates");
    EXPECT_EQ(shader.words[0], 0x07230203u, "spirv magic");
    EXPECT(SpirvContainsOp(shader.words, 87), "image sample implicit lod");
    EXPECT(SpirvContainsOp(shader.words, 246), "pc dispatcher loop present");
    // Compressed export path emits UnpackHalf2x16 as an ExtInst (12).
    EXPECT(SpirvContainsOp(shader.words, 12), "ext inst (unpack half)");
}

// Scalar + vector ALU flow through the dispatcher.
void TestTranslateAluFlow() {
    const std::vector<u32> dwords = {
        0xBEFC0310, // SMovB32 s124, s16
        0xBEFE0A7E, // SWqmB64 s126, s126
        0x10000010, // VMulF32 v0, s16, v0
        0xBF810000, // SEndpgm
    };
    GcnProgram program;
    std::string error;
    EXPECT(GcnDecodeProgram(dwords.data(), dwords.size(), program, error),
           "alu program decodes");

    GcnTranslateOptions options;
    options.stage = GcnSpirvStage::Vertex;
    GcnSpirvShader shader;
    EXPECT(GcnTranslateToSpirv(program, options, shader, error),
           "alu program translates");
    EXPECT(SpirvContainsOp(shader.words, 133), "fmul emitted"); // OpFMul
}

// Unsupported controls fail translation with a named cause.
void TestTranslateRejectsDpp() {
    GcnProgram program;
    GcnInstruction ins;
    ins.pc = 0;
    ins.encoding = GcnEncoding::Vop1;
    ins.opcode = "VMovB32";
    ins.words = {0x7E0002FF};
    ins.sources = {GcnOperand::Vector(1)};
    ins.destinations = {GcnOperand::Vector(0)};
    ins.control = GcnDppControl{};
    GcnInstruction end;
    end.pc = 4;
    end.encoding = GcnEncoding::Sopp;
    end.opcode = "SEndpgm";
    end.words = {0xBF810000};
    program.instructions = {ins, end};

    GcnTranslateOptions options;
    options.stage = GcnSpirvStage::Vertex;
    GcnSpirvShader shader;
    std::string error;
    EXPECT(!GcnTranslateToSpirv(program, options, shader, error),
           "dpp program rejected");
    EXPECT(error.find("DPP") != std::string::npos, "error names DPP");
}

} // namespace

int main() {
    TestSop2();
    TestSop2Literal();
    TestSop1();
    TestSopc();
    TestSopp();
    TestSopk();
    TestSmrd();
    TestSmem();
    TestVop1();
    TestVop1Readfirstlane();
    TestVop2();
    TestVopc();
    TestVop3();
    TestVop3Literal();
    TestVop3p();
    TestVintrp();
    TestDs();
    TestFlat();
    TestMubuf();
    TestMtbuf();
    TestMimg();
    TestExp();
    TestUnknownOpcodeDecodes();
    TestProgramRoundTrip();
    TestMetadataHeader();
    TestMetadataElf();
    TestSpirvGolden();
    TestSpirvDedup();
    TestTranslateVertex();
    TestTranslatePixel();
    TestTranslateAluFlow();
    TestTranslateRejectsDpp();

    std::fprintf(stderr, "shader_tests: %d checks, %d failures\n", g_checks,
                 g_failures);
    return g_failures == 0 ? 0 : 1;
}
