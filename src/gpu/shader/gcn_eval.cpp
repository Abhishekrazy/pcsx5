// RDNA2 shader scalar-state evaluator — implementation.
// Guided transliteration of SharpEmu's Gen5ShaderScalarEvaluator.cs onto
// our decoder IR and the emulator Memory service.  See gcn_eval.h.
#include "gcn_eval.h"
#include "../../memory/memory.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <stack>

namespace GPU::Shader {

namespace {

constexpr u32 kScalarRegisterCount   = 256;
constexpr u64 kRdnaWaveMask          = 0xFFFFFFFFull;
constexpr u64 kMaxBindingBytes       = 16ull * 1024 * 1024;

// Gen5InlineConstants.TryDecode — the gfx10 inline-constant table.
bool TryDecodeInlineConstant(u32 encoded, u32& value) {
    if (encoded == 125) {
        value = 0;
        return true;
    }
    if (encoded >= 128 && encoded <= 192) {
        value = encoded - 128;
        return true;
    }
    if (encoded >= 193 && encoded <= 208) {
        value = static_cast<u32>(-static_cast<s32>(encoded - 192));
        return true;
    }
    float floating_point;
    switch (encoded) {
        case 240: floating_point = 0.5f; break;
        case 241: floating_point = -0.5f; break;
        case 242: floating_point = 1.0f; break;
        case 243: floating_point = -1.0f; break;
        case 244: floating_point = 2.0f; break;
        case 245: floating_point = -2.0f; break;
        case 246: floating_point = 4.0f; break;
        case 247: floating_point = -4.0f; break;
        case 248: floating_point = 0.15915493679046631f; break;
        default: return false;
    }
    static_assert(sizeof(value) == sizeof(floating_point));
    std::memcpy(&value, &floating_point, sizeof(value));
    return true;
}

// Gfx10UnifiedFormat.TryDecode — RDNA2 ISA table 47 (sparse by design).
bool TryDecodeUnifiedFormat(u32 unified, u32& data_format, u32& number_format) {
    switch (unified) {
        case 0:   data_format = 0;  number_format = 0; return true;
        case 1:   data_format = 1;  number_format = 0; return true;
        case 2:   data_format = 1;  number_format = 1; return true;
        case 3:   data_format = 1;  number_format = 2; return true;
        case 4:   data_format = 1;  number_format = 3; return true;
        case 5:   data_format = 1;  number_format = 4; return true;
        case 6:   data_format = 1;  number_format = 5; return true;
        case 7:   data_format = 2;  number_format = 0; return true;
        case 8:   data_format = 2;  number_format = 1; return true;
        case 9:   data_format = 2;  number_format = 2; return true;
        case 10:  data_format = 2;  number_format = 3; return true;
        case 11:  data_format = 2;  number_format = 4; return true;
        case 12:  data_format = 2;  number_format = 5; return true;
        case 13:  data_format = 2;  number_format = 7; return true;
        case 14:  data_format = 3;  number_format = 0; return true;
        case 15:  data_format = 3;  number_format = 1; return true;
        case 16:  data_format = 3;  number_format = 2; return true;
        case 17:  data_format = 3;  number_format = 3; return true;
        case 18:  data_format = 3;  number_format = 4; return true;
        case 19:  data_format = 3;  number_format = 5; return true;
        case 20:  data_format = 4;  number_format = 4; return true;
        case 21:  data_format = 4;  number_format = 5; return true;
        case 22:  data_format = 4;  number_format = 7; return true;
        case 23:  data_format = 5;  number_format = 0; return true;
        case 24:  data_format = 5;  number_format = 1; return true;
        case 25:  data_format = 5;  number_format = 2; return true;
        case 26:  data_format = 5;  number_format = 3; return true;
        case 27:  data_format = 5;  number_format = 4; return true;
        case 28:  data_format = 5;  number_format = 5; return true;
        case 29:  data_format = 5;  number_format = 7; return true;
        case 36:  data_format = 6;  number_format = 7; return true;
        case 43:  data_format = 7;  number_format = 7; return true;
        case 44:  data_format = 8;  number_format = 0; return true;
        case 45:  data_format = 8;  number_format = 1; return true;
        case 48:  data_format = 8;  number_format = 4; return true;
        case 49:  data_format = 8;  number_format = 5; return true;
        case 50:  data_format = 9;  number_format = 0; return true;
        case 51:  data_format = 9;  number_format = 1; return true;
        case 52:  data_format = 9;  number_format = 2; return true;
        case 53:  data_format = 9;  number_format = 3; return true;
        case 54:  data_format = 9;  number_format = 4; return true;
        case 55:  data_format = 9;  number_format = 5; return true;
        case 56:  data_format = 10; number_format = 0; return true;
        case 57:  data_format = 10; number_format = 1; return true;
        case 58:  data_format = 10; number_format = 2; return true;
        case 59:  data_format = 10; number_format = 3; return true;
        case 60:  data_format = 10; number_format = 4; return true;
        case 61:  data_format = 10; number_format = 5; return true;
        case 62:  data_format = 11; number_format = 4; return true;
        case 63:  data_format = 11; number_format = 5; return true;
        case 64:  data_format = 11; number_format = 7; return true;
        case 65:  data_format = 12; number_format = 0; return true;
        case 66:  data_format = 12; number_format = 1; return true;
        case 67:  data_format = 12; number_format = 2; return true;
        case 68:  data_format = 12; number_format = 3; return true;
        case 69:  data_format = 12; number_format = 4; return true;
        case 70:  data_format = 12; number_format = 5; return true;
        case 71:  data_format = 12; number_format = 7; return true;
        case 72:  data_format = 13; number_format = 4; return true;
        case 73:  data_format = 13; number_format = 5; return true;
        case 74:  data_format = 13; number_format = 7; return true;
        case 75:  data_format = 14; number_format = 4; return true;
        case 76:  data_format = 14; number_format = 5; return true;
        case 77:  data_format = 14; number_format = 7; return true;
        case 128: data_format = 1;  number_format = 9; return true;
        case 129: data_format = 3;  number_format = 9; return true;
        case 130: data_format = 10; number_format = 9; return true;
        case 131: data_format = 131; number_format = 7; return true;
        case 132: data_format = 34; number_format = 7; return true;
        case 133: data_format = 16; number_format = 0; return true;
        case 134: data_format = 17; number_format = 0; return true;
        case 135: data_format = 18; number_format = 0; return true;
        case 136: data_format = 19; number_format = 0; return true;
        case 137: data_format = 137; number_format = 0; return true;
        case 138: data_format = 138; number_format = 0; return true;
        case 139: data_format = 139; number_format = 0; return true;
        case 140: data_format = 4;  number_format = 7; return true;
        case 141: data_format = 20; number_format = 0; return true;
        case 142: data_format = 20; number_format = 4; return true;
        case 143: data_format = 21; number_format = 0; return true;
        case 144: data_format = 21; number_format = 4; return true;
        case 145: data_format = 22; number_format = 4; return true;
        case 146: data_format = 22; number_format = 7; return true;
        case 147: data_format = 32; number_format = 0; return true;
        case 148: data_format = 32; number_format = 1; return true;
        case 149: data_format = 32; number_format = 4; return true;
        case 150: data_format = 32; number_format = 9; return true;
        case 151: data_format = 33; number_format = 0; return true;
        case 152: data_format = 33; number_format = 1; return true;
        case 153: data_format = 33; number_format = 4; return true;
        case 154: data_format = 33; number_format = 9; return true;
        default: return false;
    }
}

bool TryReadU32(u64 address, u32& value) {
    if (!Memory::IsReadable(address, sizeof(u32))) {
        return false;
    }
    value = Memory::Read<u32>(address);
    return true;
}

bool TryEvaluateScalarOperand(const GcnOperand& operand,
                              const std::vector<u32>& registers, u32& value) {
    if (operand.kind == GcnOperandKind::ScalarRegister &&
        operand.value < kScalarRegisterCount) {
        value = registers[operand.value];
        return true;
    }
    if (operand.kind == GcnOperandKind::LiteralConstant) {
        value = operand.value;
        return true;
    }
    if (operand.kind == GcnOperandKind::EncodedConstant) {
        return TryDecodeInlineConstant(operand.value, value);
    }
    value = 0;
    return false;
}

// Wave-state-aware operand evaluation (VCCZ/EXECZ/SCC encodings).
bool TryEvaluateScalarOperand(const GcnOperand& operand,
                              const std::vector<u32>& registers,
                              u64 exec_mask, bool scc, u32& value) {
    if (operand.kind == GcnOperandKind::EncodedConstant) {
        switch (operand.value) {
            case 251: // VCCZ
                value = (registers[106] | registers[107]) == 0 ? 1u : 0u;
                return true;
            case 252: // EXECZ
                value = exec_mask == 0 ? 1u : 0u;
                return true;
            case 253: // SCC
                value = scc ? 1u : 0u;
                return true;
            default:
                break;
        }
    }
    return TryEvaluateScalarOperand(operand, registers, value);
}

bool TryEvaluateScalarOperand64(const GcnOperand& operand,
                                const std::vector<u32>& registers,
                                u64 exec_mask, u64& value) {
    if (operand.kind == GcnOperandKind::ScalarRegister && operand.value == 126) {
        value = exec_mask;
        return true;
    }
    if (operand.kind == GcnOperandKind::ScalarRegister &&
        operand.value < kScalarRegisterCount - 1) {
        value = registers[operand.value] |
                (static_cast<u64>(registers[operand.value + 1]) << 32);
        return true;
    }
    u32 low = 0;
    if (TryEvaluateScalarOperand(operand, registers, low)) {
        value = operand.kind == GcnOperandKind::EncodedConstant &&
                operand.value >= 193 && operand.value <= 208
            ? 0xFFFFFFFF00000000ull | low
            : low;
        return true;
    }
    value = 0;
    return false;
}

void WriteScalarPair(std::vector<u32>& registers, u32 destination,
                     u64 value, u64& exec_mask) {
    if (destination >= kScalarRegisterCount - 1) {
        return;
    }
    if (destination == 126) {
        value &= kRdnaWaveMask;
    }
    registers[destination] = static_cast<u32>(value);
    registers[destination + 1] = static_cast<u32>(value >> 32);
    if (destination == 126) {
        exec_mask = value;
    }
}

bool SignedAddOverflow(u32 left, u32 right, u32 result) {
    return ((left ^ result) & (right ^ result) & 0x80000000u) != 0;
}

bool SignedSubOverflow(u32 left, u32 right, u32 result) {
    return ((left ^ right) & (left ^ result) & 0x80000000u) != 0;
}

u32 ReverseBits(u32 value) {
    value = (value >> 1 & 0x55555555u) | ((value & 0x55555555u) << 1);
    value = (value >> 2 & 0x33333333u) | ((value & 0x33333333u) << 2);
    value = (value >> 4 & 0x0F0F0F0Fu) | ((value & 0x0F0F0F0Fu) << 4);
    value = (value >> 8 & 0x00FF00FFu) | ((value & 0x00FF00FFu) << 8);
    return value >> 16 | value << 16;
}

bool TryGetSoppBranchTargetPc(const GcnInstruction& instruction, u32& target_pc) {
    target_pc = 0;
    if (instruction.encoding != GcnEncoding::Sopp || instruction.words.empty()) {
        return false;
    }
    const s16 offset = static_cast<s16>(instruction.words[0] & 0xFFFF);
    const s64 next_pc = static_cast<s64>(instruction.pc) +
        static_cast<s64>(instruction.words.size()) * 4;
    const s64 target = next_pc + static_cast<s64>(offset) * 4;
    if (target < 0 || target > 0xFFFFFFFFll) {
        return false;
    }
    target_pc = static_cast<u32>(target);
    return true;
}

// FNV-1a over the scalar state for the visited-path set.
u64 ComputeScalarStateHash(const std::vector<u32>& registers,
                           u64 exec_mask, bool scc) {
    constexpr u64 kPrime = 1099511628211ull;
    u64 hash = 14695981039346656037ull;
    for (const u32 value : registers) {
        hash = (hash ^ value) * kPrime;
    }
    hash = (hash ^ exec_mask) * kPrime;
    return (hash ^ (scc ? 1ull : 0ull)) * kPrime;
}

} // namespace

bool GcnTryDecodeBufferDescriptor(const std::vector<u32>& scalar_registers,
                                  u32 base, bool strict_type,
                                  GcnBufferDescriptor& out) {
    out = GcnBufferDescriptor{};
    if (base + 3 >= scalar_registers.size()) {
        return false;
    }
    const u32 word0 = scalar_registers[base];
    const u32 word1 = scalar_registers[base + 1];
    const u32 word2 = scalar_registers[base + 2];
    const u32 word3 = scalar_registers[base + 3];
    if (word0 == 0 && word1 == 0 && word2 == 0 && word3 == 0) {
        return true; // null descriptor
    }
    const u32 type = word3 >> 30;
    if (type != 0) {
        // Image-style descriptor: strict callers reject, lenient see null.
        return !strict_type;
    }
    const u64 base_address = word0 | (static_cast<u64>(word1 & 0xFFFFu) << 32);
    const u32 stride = (word1 >> 16) & 0x3FFFu;
    const u32 unified_format = (word3 >> 12) & 0x7Fu;
    u32 data_format = 0;
    u32 number_format = 0;
    if (!TryDecodeUnifiedFormat(unified_format, data_format, number_format)) {
        return false;
    }
    out.base_address = base_address;
    out.stride = stride;
    out.num_records = word2;
    out.size_bytes = stride == 0 ? word2 : static_cast<u64>(stride) * word2;
    out.number_format = number_format;
    out.data_format = data_format;
    return true;
}

} // namespace GPU::Shader

namespace GPU::Shader {

namespace {

// ---------------------------------------------------------------------------
// Scalar compares (SOPC + SCmpk).
// ---------------------------------------------------------------------------
bool TryExecuteScalarCompare(const GcnInstruction& instruction,
                             std::vector<u32>& registers,
                             bool& scc, std::string& error) {
    scc = false;
    if (instruction.sources.size() != 2) {
        error = "scalar-compare-source";
        return false;
    }
    u32 left = 0, right = 0;
    if (!TryEvaluateScalarOperand(instruction.sources[0], registers, left) ||
        !TryEvaluateScalarOperand(instruction.sources[1], registers, right)) {
        error = "scalar-compare-source";
        return false;
    }
    const std::string& opcode = instruction.opcode;
    if (opcode == "SBitcmp0B32" || opcode == "SBitcmp1B32") {
        const int bit = static_cast<int>(right & 31u);
        const bool is_set = ((left >> bit) & 1u) != 0;
        scc = opcode == "SBitcmp1B32" ? is_set : !is_set;
        return true;
    }
    if (opcode == "SBitcmp0B64" || opcode == "SBitcmp1B64") {
        u64 wide = 0;
        if (!TryEvaluateScalarOperand64(instruction.sources[0], registers, 0, wide)) {
            error = "scalar-bitcmp-source64";
            return false;
        }
        const int bit = static_cast<int>(right & 63u);
        const bool is_set = ((wide >> bit) & 1ull) != 0;
        scc = opcode == "SBitcmp1B64" ? is_set : !is_set;
        return true;
    }

    if (opcode == "SCmpEqI32")      scc = static_cast<s32>(left) == static_cast<s32>(right);
    else if (opcode == "SCmpLgI32") scc = static_cast<s32>(left) != static_cast<s32>(right);
    else if (opcode == "SCmpGtI32") scc = static_cast<s32>(left) >  static_cast<s32>(right);
    else if (opcode == "SCmpGeI32") scc = static_cast<s32>(left) >= static_cast<s32>(right);
    else if (opcode == "SCmpLtI32") scc = static_cast<s32>(left) <  static_cast<s32>(right);
    else if (opcode == "SCmpLeI32") scc = static_cast<s32>(left) <= static_cast<s32>(right);
    else if (opcode == "SCmpEqU32") scc = left == right;
    else if (opcode == "SCmpLgU32") scc = left != right;
    else if (opcode == "SCmpGtU32") scc = left >  right;
    else if (opcode == "SCmpGeU32") scc = left >= right;
    else if (opcode == "SCmpLtU32") scc = left <  right;
    else if (opcode == "SCmpLeU32") scc = left <= right;
    else {
        error = "unsupported-scalar-compare " + opcode;
        return false;
    }
    return true;
}

bool TryExecuteScalarCompareK(const GcnInstruction& instruction,
                              std::vector<u32>& registers,
                              bool& scc, std::string& error) {
    scc = false;
    if (instruction.destinations.size() != 1 ||
        instruction.destinations[0].kind != GcnOperandKind::ScalarRegister ||
        instruction.destinations[0].value >= kScalarRegisterCount) {
        error = "scalar-comparek-destination";
        return false;
    }
    const u32 left = registers[instruction.destinations[0].value];
    const u32 right = static_cast<u32>(
        static_cast<s16>(instruction.sources[0].value));
    const std::string& opcode = instruction.opcode;
    if (opcode == "SCmpkEqI32")      scc = static_cast<s32>(left) == static_cast<s32>(right);
    else if (opcode == "SCmpkLgI32") scc = static_cast<s32>(left) != static_cast<s32>(right);
    else if (opcode == "SCmpkGtI32") scc = static_cast<s32>(left) >  static_cast<s32>(right);
    else if (opcode == "SCmpkGeI32") scc = static_cast<s32>(left) >= static_cast<s32>(right);
    else if (opcode == "SCmpkLtI32") scc = static_cast<s32>(left) <  static_cast<s32>(right);
    else if (opcode == "SCmpkLeI32") scc = static_cast<s32>(left) <= static_cast<s32>(right);
    else if (opcode == "SCmpkEqU32") scc = left == right;
    else if (opcode == "SCmpkLgU32") scc = left != right;
    else if (opcode == "SCmpkGtU32") scc = left >  right;
    else if (opcode == "SCmpkGeU32") scc = left >= right;
    else if (opcode == "SCmpkLtU32") scc = left <  right;
    else if (opcode == "SCmpkLeU32") scc = left <= right;
    else {
        error = "unsupported-scalar-comparek " + opcode;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// saveexec (B32 wave32 form + B64).
// ---------------------------------------------------------------------------
bool TryExecuteSaveExecScalarAlu(const GcnInstruction& instruction,
                                 std::vector<u32>& registers,
                                 u64& exec_mask, bool& scc, std::string& error) {
    const std::string& opcode = instruction.opcode;
    const bool b32 = opcode.size() > 11 &&
        opcode.compare(opcode.size() - 11, 11, "SaveexecB32") == 0;
    if (b32) {
        if (instruction.destinations.size() != 1 ||
            instruction.destinations[0].kind != GcnOperandKind::ScalarRegister ||
            instruction.sources.empty()) {
            error = "scalar-source32";
            return false;
        }
        u32 source = 0;
        if (!TryEvaluateScalarOperand(instruction.sources[0], registers, source)) {
            error = "scalar-source32";
            return false;
        }
        const u32 old_exec = static_cast<u32>(exec_mask);
        u32 new_exec;
        if (opcode == "SAndSaveexecB32")       new_exec = old_exec & source;
        else if (opcode == "SOrSaveexecB32")   new_exec = old_exec | source;
        else if (opcode == "SXorSaveexecB32")  new_exec = old_exec ^ source;
        else if (opcode == "SAndn1SaveexecB32") new_exec = ~source & old_exec;
        else if (opcode == "SAndn2SaveexecB32") new_exec = source & ~old_exec;
        else if (opcode == "SOrn1SaveexecB32")  new_exec = ~source | old_exec;
        else if (opcode == "SOrn2SaveexecB32")  new_exec = source | ~old_exec;
        else if (opcode == "SNandSaveexecB32")  new_exec = ~(source & old_exec);
        else if (opcode == "SNorSaveexecB32")   new_exec = ~(source | old_exec);
        else                                    new_exec = ~(old_exec ^ source); // SXnor
        registers[instruction.destinations[0].value] = old_exec;
        exec_mask = new_exec;
        registers[126] = new_exec;
        registers[127] = 0;
        scc = new_exec != 0;
        return true;
    }

    const bool is_b64_saveexec =
        opcode == "SAndSaveexecB64" || opcode == "SOrSaveexecB64" ||
        opcode == "SXorSaveexecB64" || opcode == "SAndn2SaveexecB64" ||
        opcode == "SOrn2SaveexecB64" || opcode == "SNandSaveexecB64" ||
        opcode == "SNorSaveexecB64" || opcode == "SXnorSaveexecB64" ||
        opcode == "SAndn1SaveexecB64" || opcode == "SOrn1SaveexecB64";
    if (!is_b64_saveexec) {
        return false;
    }
    if (instruction.destinations.size() != 1 ||
        instruction.destinations[0].kind != GcnOperandKind::ScalarRegister ||
        instruction.destinations[0].value >= kScalarRegisterCount - 1 ||
        instruction.sources.empty()) {
        error = "scalar-source64";
        return false;
    }
    u64 source = 0;
    if (!TryEvaluateScalarOperand64(instruction.sources[0], registers, exec_mask, source)) {
        error = "scalar-source64";
        return false;
    }
    const u64 old_exec = exec_mask;
    u64 new_exec;
    if (opcode == "SAndSaveexecB64")       new_exec = old_exec & source;
    else if (opcode == "SOrSaveexecB64")   new_exec = old_exec | source;
    else if (opcode == "SXorSaveexecB64")  new_exec = old_exec ^ source;
    else if (opcode == "SAndn1SaveexecB64") new_exec = ~source & old_exec;
    else if (opcode == "SAndn2SaveexecB64") new_exec = source & ~old_exec;
    else if (opcode == "SOrn1SaveexecB64")  new_exec = ~source | old_exec;
    else if (opcode == "SOrn2SaveexecB64")  new_exec = source | ~old_exec;
    else if (opcode == "SNandSaveexecB64")  new_exec = ~(source & old_exec);
    else if (opcode == "SNorSaveexecB64")   new_exec = ~(source | old_exec);
    else                                    new_exec = ~(old_exec ^ source); // SXnor

    WriteScalarPair(registers, instruction.destinations[0].value, old_exec, exec_mask);
    exec_mask = new_exec & kRdnaWaveMask;
    WriteScalarPair(registers, 126, exec_mask, exec_mask);
    scc = exec_mask != 0;
    return true;
}

} // namespace
} // namespace GPU::Shader

namespace GPU::Shader {

namespace {

// ---------------------------------------------------------------------------
// Scalar ALU execution (TryExecuteScalarAlu).  Covers the corpus scalar set;
// unknown opcodes fail with a named error.
// ---------------------------------------------------------------------------
bool TryExecuteScalarAlu(const GcnInstruction& instruction,
                         u64 program_address,
                         std::vector<u32>& registers,
                         u64& exec_mask, bool& scc,
                         std::string& error) {
    const std::string& opcode = instruction.opcode;
    if (instruction.destinations.size() != 1 ||
        instruction.destinations[0].kind != GcnOperandKind::ScalarRegister ||
        instruction.destinations[0].value >= kScalarRegisterCount) {
        error = "unsupported-scalar-destination " + opcode;
        return false;
    }
    const u32 destination = instruction.destinations[0].value;

    if (opcode == "SMovkI32") {
        registers[destination] = static_cast<u32>(
            static_cast<s16>(instruction.sources[0].value));
        return true;
    }
    if (opcode == "SAddkI32" || opcode == "SMulkI32") {
        const u32 immediate = static_cast<u32>(
            static_cast<s16>(instruction.sources[0].value));
        registers[destination] = opcode == "SAddkI32"
            ? registers[destination] + immediate
            : registers[destination] * immediate;
        return true;
    }
    if (opcode == "SGetpcB64") {
        const u64 pc = program_address + instruction.pc +
            static_cast<u64>(instruction.words.size()) * sizeof(u32);
        WriteScalarPair(registers, destination, pc, exec_mask);
        return true;
    }

    if (TryExecuteSaveExecScalarAlu(instruction, registers, exec_mask, scc, error)) {
        return true;
    }
    if (!error.empty()) {
        return false;
    }

    // ---- 64-bit unary / shift / bitfield --------------------------------
    if (opcode == "SMovB64" || opcode == "SWqmB64" || opcode == "SNotB64") {
        if (destination >= kScalarRegisterCount - 1) {
            error = "scalar-source64 " + opcode;
            return false;
        }
        u64 value = 0;
        if (!TryEvaluateScalarOperand64(instruction.sources[0], registers, exec_mask, value)) {
            error = "scalar-source64 " + opcode;
            return false;
        }
        if (opcode == "SNotB64") {
            value = ~value;
        } else if (opcode == "SWqmB64") {
            value = (value | (value >> 1) | (value >> 2) | (value >> 3)) &
                    0x1111111111111111ull;
            value *= 0xFull;
        }
        WriteScalarPair(registers, destination, value, exec_mask);
        scc = value != 0;
        return true;
    }

    if (opcode == "SLshlB64" || opcode == "SLshrB64") {
        if (instruction.sources.size() < 2 || destination >= kScalarRegisterCount - 1) {
            error = "scalar-source64 " + opcode;
            return false;
        }
        u64 value = 0;
        u32 shift = 0;
        if (!TryEvaluateScalarOperand64(instruction.sources[0], registers, exec_mask, value) ||
            !TryEvaluateScalarOperand(instruction.sources[1], registers, shift)) {
            error = "scalar-source64 " + opcode;
            return false;
        }
        value = opcode == "SLshlB64"
            ? value << (shift & 63)
            : value >> (shift & 63);
        WriteScalarPair(registers, destination, value, exec_mask);
        scc = value != 0;
        return true;
    }

    if (opcode == "SBfeU64" || opcode == "SBfeI64") {
        if (instruction.sources.size() < 2 || destination >= kScalarRegisterCount - 1) {
            error = "scalar-source64 " + opcode;
            return false;
        }
        u64 source = 0;
        u32 control = 0;
        if (!TryEvaluateScalarOperand64(instruction.sources[0], registers, exec_mask, source) ||
            !TryEvaluateScalarOperand(instruction.sources[1], registers, control)) {
            error = "scalar-source64 " + opcode;
            return false;
        }
        const u32 offset = control & 63;
        const u32 width =
            std::min((control >> 16) & 0x7Fu, 64u - offset);
        u64 value = 0;
        if (width != 0) {
            value = source >> offset;
            if (width < 64) {
                value &= 0xFFFFFFFFFFFFFFFFull >> (64 - width);
                if (opcode == "SBfeI64") {
                    value = static_cast<u64>(
                        static_cast<s64>(value << (64 - width)) >> (64 - width));
                }
            }
        }
        WriteScalarPair(registers, destination, value, exec_mask);
        scc = value != 0;
        return true;
    }

    if (opcode == "SBfmB64") {
        if (instruction.sources.size() < 2 || destination >= kScalarRegisterCount - 1) {
            error = "scalar-source64 " + opcode;
            return false;
        }
        u32 width_source = 0, offset_source = 0;
        if (!TryEvaluateScalarOperand(instruction.sources[0], registers, width_source) ||
            !TryEvaluateScalarOperand(instruction.sources[1], registers, offset_source)) {
            error = "scalar-source64 " + opcode;
            return false;
        }
        const u32 width = width_source & 63;
        const u32 offset = offset_source & 63;
        const u64 value = width == 0
            ? 0ull
            : (0xFFFFFFFFFFFFFFFFull >> (64 - width)) << offset;
        WriteScalarPair(registers, destination, value, exec_mask);
        scc = value != 0;
        return true;
    }

    if (opcode == "SCselectB64" || opcode == "SAndB64" || opcode == "SOrB64" ||
        opcode == "SXorB64" || opcode == "SAndn2B64" || opcode == "SOrn2B64" ||
        opcode == "SNandB64" || opcode == "SNorB64" || opcode == "SXnorB64") {
        if (instruction.sources.size() < 2 || destination >= kScalarRegisterCount - 1) {
            error = "scalar-source64 " + opcode;
            return false;
        }
        u64 left = 0, right = 0;
        if (!TryEvaluateScalarOperand64(instruction.sources[0], registers, exec_mask, left) ||
            !TryEvaluateScalarOperand64(instruction.sources[1], registers, exec_mask, right)) {
            error = "scalar-source64 " + opcode;
            return false;
        }
        u64 value;
        if (opcode == "SCselectB64") {
            value = scc ? left : right;
            WriteScalarPair(registers, destination, value, exec_mask);
            return true; // cselect does not set SCC
        }
        if (opcode == "SAndB64")       value = left & right;
        else if (opcode == "SOrB64")   value = left | right;
        else if (opcode == "SXorB64")  value = left ^ right;
        else if (opcode == "SAndn2B64") value = left & ~right;
        else if (opcode == "SOrn2B64")  value = left | ~right;
        else if (opcode == "SNandB64")  value = ~(left & right);
        else if (opcode == "SNorB64")   value = ~(left | right);
        else                            value = ~(left ^ right);
        WriteScalarPair(registers, destination, value, exec_mask);
        scc = value != 0;
        return true;
    }

    // ---- 32-bit ops ------------------------------------------------------
    u32 left = 0;
    if (!TryEvaluateScalarOperand(instruction.sources[0], registers, left)) {
        error = "scalar-source32 " + opcode;
        return false;
    }

    if (opcode == "SMovB32") {
        registers[destination] = left;
        return true;
    }
    if (opcode == "SNotB32") {
        registers[destination] = ~left;
        scc = registers[destination] != 0;
        return true;
    }
    if (opcode == "SBrevB32") {
        registers[destination] = ReverseBits(left);
        scc = registers[destination] != 0;
        return true;
    }
    if (opcode == "SBcnt1I32B32") {
        u32 v = left;
        v = v - ((v >> 1) & 0x55555555u);
        v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
        registers[destination] = (((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
        scc = registers[destination] != 0;
        return true;
    }
    if (opcode == "SFF1I32B32") {
        u32 index = 0;
        if (left != 0) {
            while ((left & 1u) == 0) {
                left >>= 1;
                ++index;
            }
        }
        registers[destination] = index;
        scc = registers[destination] != 0;
        return true;
    }
    if (opcode == "SBitset1B32") {
        registers[destination] |= 1u << (left & 31u);
        return true;
    }

    if (instruction.sources.size() < 2) {
        error = "scalar-source32 " + opcode;
        return false;
    }
    u32 right = 0;
    if (!TryEvaluateScalarOperand(instruction.sources[1], registers, right)) {
        error = "scalar-source32 " + opcode;
        return false;
    }

    if (opcode == "SAddU32") {
        const u32 result = left + right;
        registers[destination] = result;
        scc = result < left;
        return true;
    }
    if (opcode == "SSubU32") {
        registers[destination] = left - right;
        scc = right > left;
        return true;
    }
    if (opcode == "SAddI32") {
        const u32 result = left + right;
        registers[destination] = result;
        scc = SignedAddOverflow(left, right, result);
        return true;
    }
    if (opcode == "SSubI32") {
        const u32 result = left - right;
        registers[destination] = result;
        scc = SignedSubOverflow(left, right, result);
        return true;
    }
    if (opcode == "SMulI32") {
        registers[destination] = left * right;
        return true;
    }
    if (opcode == "SMulHiU32") {
        registers[destination] = static_cast<u32>(
            (static_cast<u64>(left) * right) >> 32);
        return true;
    }
    if (opcode == "SAndB32") {
        registers[destination] = left & right;
        scc = registers[destination] != 0;
        return true;
    }
    if (opcode == "SOrB32") {
        registers[destination] = left | right;
        scc = registers[destination] != 0;
        return true;
    }
    if (opcode == "SXorB32") {
        registers[destination] = left ^ right;
        scc = registers[destination] != 0;
        return true;
    }
    if (opcode == "SAndn2B32") {
        registers[destination] = left & ~right;
        scc = registers[destination] != 0;
        return true;
    }
    if (opcode == "SOrn2B32") {
        registers[destination] = left | ~right;
        scc = registers[destination] != 0;
        return true;
    }
    if (opcode == "SNandB32") {
        registers[destination] = ~(left & right);
        scc = registers[destination] != 0;
        return true;
    }
    if (opcode == "SNorB32") {
        registers[destination] = ~(left | right);
        scc = registers[destination] != 0;
        return true;
    }
    if (opcode == "SXnorB32") {
        registers[destination] = ~(left ^ right);
        scc = registers[destination] != 0;
        return true;
    }
    if (opcode == "SLshlB32") {
        registers[destination] = left << (right & 31u);
        scc = registers[destination] != 0;
        return true;
    }
    if (opcode == "SLshrB32") {
        registers[destination] = left >> (right & 31u);
        scc = registers[destination] != 0;
        return true;
    }
    if (opcode == "SAshrI32") {
        registers[destination] = static_cast<u32>(
            static_cast<s32>(left) >> (right & 31u));
        scc = registers[destination] != 0;
        return true;
    }
    if (opcode == "SBfmB32") {
        const u32 width = left & 31u;
        const u32 offset = right & 31u;
        registers[destination] = width == 0
            ? 0
            : (0xFFFFFFFFu >> (32 - width)) << offset;
        return true;
    }
    if (opcode == "SBfeU32" || opcode == "SBfeI32") {
        const u32 offset = right & 31u;
        const u32 width = std::min((right >> 16) & 0x7Fu, 32u - offset);
        u32 value = 0;
        if (width != 0) {
            value = left >> offset;
            if (width < 32) {
                value &= 0xFFFFFFFFu >> (32 - width);
                if (opcode == "SBfeI32") {
                    value = static_cast<u32>(
                        static_cast<s32>(value << (32 - width)) >> (32 - width));
                }
            }
        }
        registers[destination] = value;
        scc = value != 0;
        return true;
    }
    if (opcode == "SCselectB32") {
        registers[destination] = scc ? left : right;
        return true;
    }
    if (opcode == "SMinU32") {
        registers[destination] = std::min(left, right);
        scc = left < right;
        return true;
    }
    if (opcode == "SMaxU32") {
        registers[destination] = std::max(left, right);
        scc = left > right;
        return true;
    }
    if (opcode == "SMinI32") {
        registers[destination] = static_cast<u32>(
            std::min(static_cast<s32>(left), static_cast<s32>(right)));
        scc = static_cast<s32>(left) < static_cast<s32>(right);
        return true;
    }
    if (opcode == "SMaxI32") {
        registers[destination] = static_cast<u32>(
            std::max(static_cast<s32>(left), static_cast<s32>(right)));
        scc = static_cast<s32>(left) > static_cast<s32>(right);
        return true;
    }
    if (opcode == "SLshl1AddU32" || opcode == "SLshl2AddU32" ||
        opcode == "SLshl3AddU32" || opcode == "SLshl4AddU32") {
        const u32 shift = static_cast<u32>(opcode[5] - '0');
        registers[destination] = (left << shift) + right;
        return true;
    }

    error = "unsupported scalar opcode " + opcode;
    return false;
}

} // namespace
} // namespace GPU::Shader

namespace GPU::Shader {

namespace {

bool IsBufferMemoryWrite(const std::string& opcode) {
    return opcode.rfind("BufferStore", 0) == 0 ||
           opcode.rfind("TBufferStore", 0) == 0 ||
           opcode.rfind("BufferAtomic", 0) == 0 ||
           opcode.rfind("TBufferAtomic", 0) == 0;
}

bool UsesSampler(const std::string& opcode) {
    return opcode.rfind("ImageSample", 0) == 0 ||
           opcode.rfind("ImageGather", 0) == 0;
}

// Records (or extends) a buffer binding for (scalar_address, base_address).
void RecordBufferBinding(
    GcnEvaluation& eval,
    std::map<std::pair<u32, u64>, size_t>& by_address,
    u32 scalar_address, u64 base_address, u64 size_bytes, u32 pc,
    bool writable) {
    const auto key = std::make_pair(scalar_address, base_address);
    if (const auto it = by_address.find(key); it != by_address.end()) {
        GcnEvalBufferBinding& binding = eval.buffer_bindings[it->second];
        binding.writable |= writable;
        if (std::find(binding.instruction_pcs.begin(),
                      binding.instruction_pcs.end(), pc) ==
            binding.instruction_pcs.end()) {
            binding.instruction_pcs.push_back(pc);
        }
        return;
    }
    GcnEvalBufferBinding binding;
    binding.scalar_address = scalar_address;
    binding.base_address = base_address;
    binding.size_bytes = std::min(size_bytes, kMaxBindingBytes);
    binding.writable = writable;
    binding.instruction_pcs.push_back(pc);
    by_address.emplace(key, eval.buffer_bindings.size());
    eval.buffer_bindings.push_back(std::move(binding));
}

// Scalar loads (SLoadDword*/SBufferLoadDword*): evaluate the load through
// guest memory so downstream descriptor-carrying registers materialize.
bool TryExecuteScalarLoad(const GcnInstruction& instruction,
                          const GcnScalarMemoryControl& control,
                          std::vector<u32>& registers,
                          GcnEvaluation& eval,
                          std::map<std::pair<u32, u64>, size_t>& by_address,
                          bool record_binding,
                          std::string& error) {
    if (instruction.sources.empty() ||
        instruction.sources[0].kind != GcnOperandKind::ScalarRegister ||
        instruction.sources[0].value >= kScalarRegisterCount - 1) {
        error = "invalid-scalar-base";
        return false;
    }
    const u32 scalar_base = instruction.sources[0].value;
    const bool is_buffer_load = instruction.opcode.rfind("SBufferLoad", 0) == 0;

    GcnBufferDescriptor descriptor;
    const bool has_descriptor = is_buffer_load &&
        GcnTryDecodeBufferDescriptor(registers, scalar_base, false, descriptor);
    const u64 base_address = has_descriptor
        ? descriptor.base_address
        : registers[scalar_base] |
          (static_cast<u64>(registers[scalar_base + 1]) << 32);
    const u32 dynamic_offset = control.dynamic_offset_register.has_value() &&
                               *control.dynamic_offset_register < kScalarRegisterCount
        ? registers[*control.dynamic_offset_register]
        : 0;
    const u64 byte_offset = static_cast<u64>(
        static_cast<s64>(control.immediate_offset_bytes)) + dynamic_offset;
    const u64 address = (base_address + byte_offset) & ~3ull;

    const bool buffer_unbound = is_buffer_load &&
        (!has_descriptor || descriptor.size_bytes == 0 ||
         (registers[scalar_base] == 0 && registers[scalar_base + 1] == 0 &&
          scalar_base + 3 < kScalarRegisterCount &&
          registers[scalar_base + 2] == 0 && registers[scalar_base + 3] == 0));
    const u64 buffer_size =
        is_buffer_load ? descriptor.size_bytes : 0xFFFFFFFFFFFFFFFFull;

    if (record_binding && base_address != 0 && !buffer_unbound) {
        const u64 size = is_buffer_load
            ? descriptor.size_bytes
            : std::max(256ull * 1024,
                       byte_offset +
                           static_cast<u64>(
                               std::max<size_t>(instruction.destinations.size(), 1)) * 4);
        RecordBufferBinding(eval, by_address, scalar_base, base_address, size,
                            instruction.pc, /*writable=*/false);
    }

    for (size_t index = 0; index < instruction.destinations.size(); ++index) {
        const auto& destination = instruction.destinations[index];
        if (destination.kind != GcnOperandKind::ScalarRegister ||
            destination.value >= kScalarRegisterCount) {
            error = "invalid-scalar-destination";
            return false;
        }
        const u64 component_offset = byte_offset + index * sizeof(u32);
        u32 value = 0;
        if (!buffer_unbound &&
            !(is_buffer_load &&
              (component_offset >= buffer_size ||
               buffer_size - component_offset < sizeof(u32)))) {
            TryReadU32(address + index * sizeof(u32), value); // unreadable -> 0
        }
        registers[destination.value] = value;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Path state for the symbolic walk.
// ---------------------------------------------------------------------------
struct ScalarPathState {
    u32 start_pc = 0;
    std::vector<u32> registers;
    u64 exec_mask = 0;
    bool scc = false;
    bool supplemental = false;
};

} // namespace

// ---------------------------------------------------------------------------
// GcnEvaluateScalarState — the symbolic scalar walk (TryEvaluate).
// ---------------------------------------------------------------------------
bool GcnEvaluateScalarState(const GcnProgram&    program,
                            u64                  program_address,
                            const std::vector<u32>& user_data,
                            u32                  user_data_scalar_register_base,
                            GcnEvaluation&       out,
                            std::string&         error) {
    error.clear();
    out = GcnEvaluation{};

    std::vector<u32> scalar_registers(kScalarRegisterCount, 0);
    for (u32 index = 0;
         index < user_data.size() &&
         user_data_scalar_register_base + index < kScalarRegisterCount;
         ++index) {
        scalar_registers[user_data_scalar_register_base + index] = user_data[index];
    }

    u64 exec_mask = kRdnaWaveMask;
    WriteScalarPair(scalar_registers, 106, 0, exec_mask);
    WriteScalarPair(scalar_registers, 126, exec_mask, exec_mask);
    out.initial_scalar_registers = scalar_registers;
    out.scalar_registers = scalar_registers;

    std::map<std::pair<u32, u64>, size_t> buffer_by_address;
    std::map<u32, size_t> image_by_pc;
    std::stack<ScalarPathState> pending_paths;
    std::set<std::pair<u32, u64>> visited_paths;

    auto queue_path = [&](u32 pc, const std::vector<u32>& registers,
                          u64 path_exec, bool path_scc, bool supplemental) {
        const u64 hash = ComputeScalarStateHash(registers, path_exec, path_scc);
        if (visited_paths.emplace(pc, hash).second) {
            pending_paths.push(
                ScalarPathState{pc, registers, path_exec, path_scc, supplemental});
        }
    };

    const auto& instructions = program.instructions;
    if (instructions.empty()) {
        return true;
    }
    queue_path(instructions[0].pc, scalar_registers, exec_mask, false, false);

    while (!pending_paths.empty()) {
        ScalarPathState path = pending_paths.top();
        pending_paths.pop();
        scalar_registers = path.registers;
        exec_mask = path.exec_mask;
        bool scc = path.scc;

        u32 skip_until_pc = path.start_pc;
        bool have_skip = true;

        for (const GcnInstruction& instruction : instructions) {
            if (have_skip) {
                if (instruction.pc < skip_until_pc) {
                    continue;
                }
                have_skip = false;
            }

            if (instruction.opcode == "SEndpgm") {
                break;
            }

            if (instruction.opcode == "SBranch") {
                u32 target_pc = 0;
                if (TryGetSoppBranchTargetPc(instruction, target_pc)) {
                    if (target_pc > instruction.pc) {
                        // Forward branch: follow it, and re-visit the skipped
                        // region once as supplemental resource discovery.
                        const u32 fallthrough_pc =
                            instruction.pc +
                            static_cast<u32>(instruction.words.size()) * 4;
                        queue_path(fallthrough_pc, scalar_registers, exec_mask, scc,
                                   /*supplemental=*/true);
                        skip_until_pc = target_pc;
                        have_skip = true;
                        continue;
                    }
                    if (path.supplemental) {
                        break; // one linear pass over a supplemental body
                    }
                    skip_until_pc = target_pc;
                    have_skip = true;
                    continue;
                }
            }

            if (instruction.encoding == GcnEncoding::Sopc) {
                if (!TryExecuteScalarCompare(instruction, scalar_registers, scc, error)) {
                    return false;
                }
                continue;
            }

            if (instruction.encoding == GcnEncoding::Sopk &&
                instruction.opcode.rfind("SCmpk", 0) == 0) {
                if (!TryExecuteScalarCompareK(instruction, scalar_registers, scc, error)) {
                    return false;
                }
                continue;
            }

            if (instruction.encoding == GcnEncoding::Sop1 ||
                instruction.encoding == GcnEncoding::Sop2 ||
                instruction.encoding == GcnEncoding::Sopk) {
                if (instruction.opcode == "SSetpcB64" ||
                    instruction.opcode == "SSwappcB64") {
                    break;
                }
                if (!TryExecuteScalarAlu(instruction, program_address,
                                         scalar_registers, exec_mask, scc, error)) {
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), " pc=0x%X", instruction.pc);
                    error += buf;
                    return false;
                }
                continue;
            }

            if (const auto* scalar_memory =
                    std::get_if<GcnScalarMemoryControl>(&instruction.control)) {
                const bool record_binding = !path.supplemental;
                if (!TryExecuteScalarLoad(instruction, *scalar_memory,
                                          scalar_registers, out, buffer_by_address,
                                          record_binding, error)) {
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), " pc=0x%X op=%s",
                                  instruction.pc, instruction.opcode.c_str());
                    error += buf;
                    return false;
                }
                continue;
            }

            if (std::get_if<GcnGlobalMemoryControl>(&instruction.control)) {
                // Global (flat) memory: not in the bring-up corpus.
                continue;
            }

            if (const auto* buffer_memory =
                    std::get_if<GcnBufferMemoryControl>(&instruction.control)) {
                const bool writable = IsBufferMemoryWrite(instruction.opcode);
                if (buffer_memory->scalar_resource >= kScalarRegisterCount - 3) {
                    error = "buffer-resource-register-range";
                    return false;
                }
                GcnBufferDescriptor descriptor;
                if (!GcnTryDecodeBufferDescriptor(scalar_registers,
                                                  buffer_memory->scalar_resource,
                                                  /*strict_type=*/true, descriptor)) {
                    char buf[96];
                    std::snprintf(buf, sizeof(buf),
                                  "buffer-descriptor-invalid pc=0x%X s%u",
                                  instruction.pc, buffer_memory->scalar_resource);
                    error = buf;
                    return false;
                }
                if (descriptor.base_address == 0) {
                    // Sibling-block null descriptor: bind a 4-byte zero buffer
                    // to this exact pc (Vulkan still needs the descriptor).
                    RecordBufferBinding(out, buffer_by_address,
                                        buffer_memory->scalar_resource, 0, 4,
                                        instruction.pc, writable);
                    continue;
                }
                RecordBufferBinding(out, buffer_by_address,
                                    buffer_memory->scalar_resource,
                                    descriptor.base_address, descriptor.size_bytes,
                                    instruction.pc, writable);
                continue;
            }

            const auto* image = std::get_if<GcnImageControl>(&instruction.control);
            if (image == nullptr) {
                continue;
            }
            if (path.supplemental && image_by_pc.count(instruction.pc) != 0) {
                continue;
            }
            if (image->scalar_resource + 8 > kScalarRegisterCount) {
                error = "resource-register-range";
                return false;
            }
            GcnEvalImageBinding binding;
            binding.pc = instruction.pc;
            binding.opcode = instruction.opcode;
            for (u32 i = 0; i < 8; ++i) {
                binding.resource_descriptor[i] =
                    scalar_registers[image->scalar_resource + i];
            }
            if (UsesSampler(instruction.opcode)) {
                if (image->scalar_sampler + 4 > kScalarRegisterCount) {
                    error = "sampler-register-range";
                    return false;
                }
                for (u32 i = 0; i < 4; ++i) {
                    binding.sampler_descriptor[i] =
                        scalar_registers[image->scalar_sampler + i];
                }
                binding.has_sampler = true;
            }
            if (const auto it = image_by_pc.find(instruction.pc);
                it != image_by_pc.end()) {
                GcnEvalImageBinding& existing = out.image_bindings[it->second];
                const bool existing_null = std::all_of(
                    existing.resource_descriptor.begin(),
                    existing.resource_descriptor.end(),
                    [](u32 word) { return word == 0; });
                const bool candidate_null = std::all_of(
                    binding.resource_descriptor.begin(),
                    binding.resource_descriptor.end(),
                    [](u32 word) { return word == 0; });
                if (existing_null && !candidate_null) {
                    existing = std::move(binding);
                } else if (!candidate_null &&
                           existing.resource_descriptor != binding.resource_descriptor) {
                    char buf[96];
                    std::snprintf(buf, sizeof(buf),
                                  "dynamic-image-descriptor pc=0x%X", instruction.pc);
                    error = buf;
                    return false;
                }
            } else {
                image_by_pc.emplace(instruction.pc, out.image_bindings.size());
                out.image_bindings.push_back(std::move(binding));
            }
        }

        if (!path.supplemental) {
            out.scalar_registers = scalar_registers;
        }
    }

    return true;
}

} // namespace GPU::Shader
