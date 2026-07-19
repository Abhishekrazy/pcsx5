// RDNA2 -> SPIR-V translator: scalar/vector ALU + compares.
// Guided transliteration of SharpEmu's Gen5SpirvTranslator.Alu.cs.
// SDWA/DPP controls do not occur in the bring-up corpus and fail
// translation loudly; VOP3 abs/neg/clamp/output modifiers are honored.
#include "gcn_translate_internal.h"

#include <cstring>

namespace GPU::Shader {

namespace {

// Gen5InlineConstants.TryDecode (SharpEmu): the gfx10 inline-constant table.
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
        case 248: floating_point = 0.15915493679046631f; break; // 1/(2*pi)
        default: return false;
    }
    u32 bits;
    static_assert(sizeof(bits) == sizeof(floating_point));
    std::memcpy(&bits, &floating_point, sizeof(bits));
    value = bits;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Operand helpers.
// ---------------------------------------------------------------------------
bool SpirvTranslateContext::TryGetVectorDestination(
    const GcnInstruction& instruction, u32& destination) {
    if (!instruction.destinations.empty() &&
        instruction.destinations[0].kind == GcnOperandKind::VectorRegister) {
        destination = instruction.destinations[0].value;
        return true;
    }
    destination = 0;
    return false;
}

u32 SpirvTranslateContext::GetRawSource(
    const GcnInstruction& instruction, int source_index,
    bool apply_sdwa_integer_modifiers) {
    const auto& operand = instruction.sources[source_index];
    u32 value = 0;
    switch (operand.kind) {
        case GcnOperandKind::VectorRegister:
            value = LoadV(operand.value);
            break;
        case GcnOperandKind::ScalarRegister:
            value = LoadS(operand.value);
            break;
        case GcnOperandKind::LiteralConstant:
            value = UInt(operand.value);
            break;
        case GcnOperandKind::EncodedConstant: {
            // VCCZ / EXECZ / SCC pseudo-registers (single-lane votes).
            if (operand.value == 251) {
                value = module_.AddInstruction(
                    SpirvOp::Select, uint_type_,
                    {LogicalNot(Load(bool_type_, vcc_)), UInt(1), UInt(0)});
            } else if (operand.value == 252) {
                value = module_.AddInstruction(
                    SpirvOp::Select, uint_type_,
                    {LogicalNot(Load(bool_type_, exec_)), UInt(1), UInt(0)});
            } else if (operand.value == 253) {
                value = module_.AddInstruction(
                    SpirvOp::Select, uint_type_,
                    {Load(bool_type_, scc_), UInt(1), UInt(0)});
            } else {
                u32 inline_value = 0;
                if (TryDecodeInlineConstant(operand.value, inline_value)) {
                    value = UInt(inline_value);
                } else {
                    value = UInt(0); // unreachable for well-formed input
                }
            }
            break;
        }
    }

    // SDWA source selectors: byte/word extraction + optional sign extend,
    // then integer abs/negate (skipped for float consumers, which apply
    // the float forms of the same masks in GetFloatSource).
    if (const auto* sdwa = std::get_if<GcnSdwaControl>(&instruction.control)) {
        const u32 selector = source_index == 0
            ? sdwa->source0_select
            : source_index == 1
                ? sdwa->source1_select
                : 6u;
        switch (selector) {
            case 0: value = BitwiseAnd(value, UInt(0xFF)); break;
            case 1: value = BitwiseAnd(ShiftRightLogical(value, UInt(8)), UInt(0xFF)); break;
            case 2: value = BitwiseAnd(ShiftRightLogical(value, UInt(16)), UInt(0xFF)); break;
            case 3: value = BitwiseAnd(ShiftRightLogical(value, UInt(24)), UInt(0xFF)); break;
            case 4: value = BitwiseAnd(value, UInt(0xFFFF)); break;
            case 5: value = BitwiseAnd(ShiftRightLogical(value, UInt(16)), UInt(0xFFFF)); break;
            default: break;
        }
        const bool sign_extend = source_index == 0
            ? sdwa->source0_sign_extend
            : source_index == 1 && sdwa->source1_sign_extend;
        if (sign_extend && selector != 6) {
            const u32 width = selector <= 3 ? 8u : 16u;
            value = Bitcast(
                uint_type_,
                module_.AddInstruction(
                    SpirvOp::BitFieldSExtract, int_type_,
                    {Bitcast(int_type_, value), UInt(0), UInt(width)}));
        }

        if (apply_sdwa_integer_modifiers) {
            if ((sdwa->absolute_mask & (1u << source_index)) != 0) {
                value = Bitcast(
                    uint_type_,
                    Ext(GlslExt::SAbs, int_type_, {Bitcast(int_type_, value)}));
            }
            if ((sdwa->negate_mask & (1u << source_index)) != 0) {
                value = module_.AddInstruction(
                    SpirvOp::ISub, uint_type_, {UInt(0), value});
            }
        }
    }
    return value;
}

u32 SpirvTranslateContext::GetRawSource64(
    const GcnInstruction& instruction, int source_index) {
    const auto& operand = instruction.sources[source_index];
    if (operand.kind == GcnOperandKind::ScalarRegister) {
        return LoadS64(operand.value);
    }
    if (operand.kind == GcnOperandKind::VectorRegister) {
        const u32 low = module_.AddInstruction(
            SpirvOp::UConvert, ulong_type_, {LoadV(operand.value)});
        u32 high = module_.AddInstruction(
            SpirvOp::UConvert, ulong_type_, {LoadV(operand.value + 1)});
        high = ShiftLeftLogical64(high, module_.Constant64(ulong_type_, 32));
        return module_.AddInstruction(SpirvOp::BitwiseOr, ulong_type_, {low, high});
    }
    // Scalar inline negative constants sign-extend into B64 consumers.
    if (operand.kind == GcnOperandKind::EncodedConstant &&
        operand.value >= 193 && operand.value <= 208) {
        const s64 signed_value = -static_cast<s64>(operand.value - 192);
        return module_.Constant64(ulong_type_, static_cast<u64>(signed_value));
    }
    const u32 low = GetRawSource(instruction, source_index);
    return module_.AddInstruction(SpirvOp::UConvert, ulong_type_, {low});
}

u32 SpirvTranslateContext::LoadS64(u32 reg) {
    const u32 low = module_.AddInstruction(SpirvOp::UConvert, ulong_type_, {LoadS(reg)});
    u32 high =
        module_.AddInstruction(SpirvOp::UConvert, ulong_type_, {LoadS(reg + 1)});
    high = ShiftLeftLogical64(high, module_.Constant64(ulong_type_, 32));
    return module_.AddInstruction(SpirvOp::BitwiseOr, ulong_type_, {low, high});
}

void SpirvTranslateContext::StoreS64(u32 reg, u32 value) {
    StoreS(reg, module_.AddInstruction(SpirvOp::UConvert, uint_type_, {value}));
    const u32 high =
        ShiftRightLogical64(value, module_.Constant64(ulong_type_, 32));
    StoreS(reg + 1, module_.AddInstruction(SpirvOp::UConvert, uint_type_, {high}));
}

u32 SpirvTranslateContext::GetFloatSource(
    const GcnInstruction& instruction, int source_index) {
    const auto& operand = instruction.sources[source_index];
    u32 value;
    if (operand.kind == GcnOperandKind::EncodedConstant &&
        operand.value >= 128 && operand.value <= 192) {
        value = Float(static_cast<float>(operand.value - 128));
    } else if (operand.kind == GcnOperandKind::EncodedConstant &&
               operand.value >= 193 && operand.value <= 208) {
        value = Float(-static_cast<float>(operand.value - 192));
    } else {
        value = Bitcast(
            float_type_,
            GetRawSource(instruction, source_index,
                         /*apply_sdwa_integer_modifiers=*/false));
    }

    u32 absolute_mask = 0;
    u32 negate_mask = 0;
    if (const auto* vop3 = std::get_if<GcnVop3Control>(&instruction.control)) {
        absolute_mask = vop3->absolute_mask;
        negate_mask = vop3->negate_mask;
    } else if (const auto* sdwa = std::get_if<GcnSdwaControl>(&instruction.control)) {
        absolute_mask = sdwa->absolute_mask;
        negate_mask = sdwa->negate_mask;
    } else if (const auto* dpp = std::get_if<GcnDppControl>(&instruction.control)) {
        absolute_mask = dpp->absolute_mask;
        negate_mask = dpp->negate_mask;
    }

    if ((absolute_mask & (1u << source_index)) != 0) {
        value = Ext(GlslExt::FAbs, float_type_, {value});
    }
    if ((negate_mask & (1u << source_index)) != 0) {
        value = module_.AddInstruction(SpirvOp::FNegate, float_type_, {value});
    }
    return value;
}

// ---------------------------------------------------------------------------
// Result helpers (VOP3 output modifier + clamp).
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// SDWA destination packing (ApplySdwaDestination): byte/word insertion with
// zero/sign-extend/preserve fill of the untouched bits.
// ---------------------------------------------------------------------------
u32 SpirvTranslateContext::ApplySdwaDestination(
    const GcnSdwaControl& control, u32 value, u32 previous) {
    u32 shift;
    u32 width;
    switch (control.destination_select) {
        case 0: shift = 0;  width = 8;  break;
        case 1: shift = 8;  width = 8;  break;
        case 2: shift = 16; width = 8;  break;
        case 3: shift = 24; width = 8;  break;
        case 4: shift = 0;  width = 16; break;
        case 5: shift = 16; width = 16; break;
        default: shift = 0; width = 32; break;
    }
    if (width == 32) {
        return value;
    }

    const u32 low_mask = width == 8 ? 0xFFu : 0xFFFFu;
    const u32 field_mask = low_mask << shift;
    const u32 upper_start = shift + width;
    const u32 upper_mask = upper_start == 32 ? 0u : 0xFFFFFFFFu << upper_start;
    const u32 positioned = ShiftLeftLogical(
        BitwiseAnd(value, UInt(low_mask)), UInt(shift));
    switch (control.destination_unused) {
        case 0:
            return positioned;
        case 1:
            return BitwiseOr(
                positioned,
                module_.AddInstruction(
                    SpirvOp::Select, uint_type_,
                    {IsNotZero(BitwiseAnd(positioned, UInt(1u << (upper_start - 1)))),
                     UInt(upper_mask), UInt(0)}));
        case 2:
            return BitwiseOr(BitwiseAnd(previous, UInt(~field_mask)), positioned);
        default:
            return positioned;
    }
}

u32 SpirvTranslateContext::EmitFloatResult(
    const GcnInstruction& instruction, u32 value) {
    u32 output_modifier = 0;
    bool clamp = false;
    if (const auto* vop3 = std::get_if<GcnVop3Control>(&instruction.control)) {
        output_modifier = vop3->output_modifier;
        clamp = vop3->clamp;
    } else if (const auto* sdwa = std::get_if<GcnSdwaControl>(&instruction.control)) {
        output_modifier = sdwa->output_modifier;
        clamp = sdwa->clamp;
    }

    switch (output_modifier) {
        case 1:
            value = module_.AddInstruction(
                SpirvOp::FMul, float_type_, {value, Float(2.f)});
            break;
        case 2:
            value = module_.AddInstruction(
                SpirvOp::FMul, float_type_, {value, Float(4.f)});
            break;
        case 3:
            value = module_.AddInstruction(
                SpirvOp::FMul, float_type_, {value, Float(0.5f)});
            break;
        default:
            break;
    }
    if (clamp) {
        value = Ext(GlslExt::FClamp, float_type_, {value, Float(0.f), Float(1.f)});
    }
    return Bitcast(uint_type_, value);
}

u32 SpirvTranslateContext::EmitFloatClamp(u32 value) {
    return Ext(GlslExt::FClamp, float_type_, {value, Float(0.f), Float(1.f)});
}

u32 SpirvTranslateContext::EmitFloatBinary(
    const GcnInstruction& instruction, SpirvOp operation, bool reverse) {
    const u32 left = GetFloatSource(instruction, reverse ? 1 : 0);
    const u32 right = GetFloatSource(instruction, reverse ? 0 : 1);
    return EmitFloatResult(
        instruction,
        module_.AddInstruction(operation, float_type_, {left, right}));
}

u32 SpirvTranslateContext::EmitFloatExtBinary(
    const GcnInstruction& instruction, u32 operation) {
    return EmitFloatResult(
        instruction,
        Ext(operation, float_type_,
            {GetFloatSource(instruction, 0), GetFloatSource(instruction, 1)}));
}

u32 SpirvTranslateContext::EmitFloatTernaryExt(
    const GcnInstruction& instruction, u32 operation) {
    const u32 first = Ext(
        operation, float_type_,
        {GetFloatSource(instruction, 0), GetFloatSource(instruction, 1)});
    return EmitFloatResult(
        instruction,
        Ext(operation, float_type_, {first, GetFloatSource(instruction, 2)}));
}

u32 SpirvTranslateContext::EmitIntegerBinary(
    const GcnInstruction& instruction, SpirvOp operation, bool reverse) {
    const u32 left = GetRawSource(instruction, reverse ? 1 : 0);
    const u32 right = GetRawSource(instruction, reverse ? 0 : 1);
    return module_.AddInstruction(operation, uint_type_, {left, right});
}

} // namespace GPU::Shader

namespace GPU::Shader {

// ---------------------------------------------------------------------------
// Carry / overflow helpers.
// ---------------------------------------------------------------------------
u32 SpirvTranslateContext::TruncateFloat32ForPack(u32 value) {
    const u32 raw = BitwiseAnd(Bitcast(uint_type_, value), UInt(0xFFFFE000u));
    return Bitcast(float_type_, raw);
}

u32 SpirvTranslateContext::SignBit(u32 value) {
    return ShiftRightLogical(value, UInt(31));
}

u32 SpirvTranslateContext::SignedAddOverflow(u32 left, u32 right, u32 result) {
    const u32 left_sign = SignBit(left);
    const u32 right_sign = SignBit(right);
    const u32 result_sign = SignBit(result);
    const u32 same_source_sign = module_.AddInstruction(
        SpirvOp::IEqual, bool_type_, {left_sign, right_sign});
    const u32 result_sign_changed = module_.AddInstruction(
        SpirvOp::INotEqual, bool_type_, {left_sign, result_sign});
    return module_.AddInstruction(
        SpirvOp::LogicalAnd, bool_type_, {same_source_sign, result_sign_changed});
}

u32 SpirvTranslateContext::SignedSubOverflow(u32 left, u32 right, u32 result) {
    const u32 left_sign = SignBit(left);
    const u32 right_sign = SignBit(right);
    const u32 result_sign = SignBit(result);
    const u32 different_source_sign = module_.AddInstruction(
        SpirvOp::INotEqual, bool_type_, {left_sign, right_sign});
    const u32 result_sign_changed = module_.AddInstruction(
        SpirvOp::INotEqual, bool_type_, {left_sign, result_sign});
    return module_.AddInstruction(
        SpirvOp::LogicalAnd, bool_type_,
        {different_source_sign, result_sign_changed});
}

void SpirvTranslateContext::StoreCarryOut(
    const GcnInstruction& instruction, u32 carry) {
    const u32 active_carry = module_.AddInstruction(
        SpirvOp::LogicalAnd, bool_type_, {Load(bool_type_, exec_), carry});
    if (const auto* control = std::get_if<GcnVop3Control>(&instruction.control)) {
        if (control->scalar_destination.has_value()) {
            StoreWaveMask(*control->scalar_destination, active_carry);
            return;
        }
    }
    StoreWaveMask(106, active_carry);
}

u32 SpirvTranslateContext::EmitAddWithCarry(const GcnInstruction& instruction) {
    const u32 left = GetRawSource(instruction, 0);
    const u32 right = GetRawSource(instruction, 1);
    const u32 carry_mask = instruction.sources.size() > 2
        ? IsWaveMaskActive(GetRawSource64(instruction, 2))
        : Load(bool_type_, vcc_);
    const u32 carry_in = module_.AddInstruction(
        SpirvOp::Select, uint_type_, {carry_mask, UInt(1), UInt(0)});
    const u32 partial = IAdd(left, right);
    const u32 result = IAdd(partial, carry_in);
    const u32 carry = module_.AddInstruction(
        SpirvOp::LogicalOr, bool_type_,
        {module_.AddInstruction(SpirvOp::ULessThan, bool_type_, {partial, left}),
         module_.AddInstruction(SpirvOp::ULessThan, bool_type_, {result, partial})});
    StoreCarryOut(instruction, carry);
    return result;
}

u32 SpirvTranslateContext::EmitSubtractWithBorrow(
    const GcnInstruction& instruction, bool reverse) {
    const u32 left = GetRawSource(instruction, reverse ? 1 : 0);
    const u32 right = GetRawSource(instruction, reverse ? 0 : 1);
    const u32 borrow_mask = instruction.sources.size() > 2
        ? IsWaveMaskActive(GetRawSource64(instruction, 2))
        : Load(bool_type_, vcc_);
    const u32 borrow_in = module_.AddInstruction(
        SpirvOp::Select, uint_type_, {borrow_mask, UInt(1), UInt(0)});
    const u32 partial = module_.AddInstruction(
        SpirvOp::ISub, uint_type_, {left, right});
    const u32 result = module_.AddInstruction(
        SpirvOp::ISub, uint_type_, {partial, borrow_in});
    const u32 borrow = module_.AddInstruction(
        SpirvOp::LogicalOr, bool_type_,
        {module_.AddInstruction(SpirvOp::ULessThan, bool_type_, {left, right}),
         module_.AddInstruction(SpirvOp::ULessThan, bool_type_, {partial, borrow_in})});
    StoreCarryOut(instruction, borrow);
    return result;
}

// ---------------------------------------------------------------------------
// Vector compares: VCC/EXEC masks are overwritten with EXEC & condition.
// ---------------------------------------------------------------------------
bool SpirvTranslateContext::TryEmitVectorCompare(
    const GcnInstruction& instruction, std::string& error) {
    u32 condition = module_.ConstantBool(false);
    const std::string& opcode = instruction.opcode;

    if (opcode == "VCmpFF32" || opcode == "VCmpxFF32" ||
        opcode == "VCmpFI32" || opcode == "VCmpFU32") {
        condition = module_.ConstantBool(false);
    } else if (opcode == "VCmpTruF32" || opcode == "VCmpxTruF32" ||
               opcode == "VCmpTI32" || opcode == "VCmpTU32") {
        condition = module_.ConstantBool(true);
    } else if (opcode == "VCmpOF32" || opcode == "VCmpxOF32" ||
               opcode == "VCmpUF32" || opcode == "VCmpxUF32") {
        const u32 left = GetFloatSource(instruction, 0);
        const u32 right = GetFloatSource(instruction, 1);
        const u32 unordered = module_.AddInstruction(
            SpirvOp::LogicalOr, bool_type_,
            {module_.AddInstruction(SpirvOp::IsNan, bool_type_, {left}),
             module_.AddInstruction(SpirvOp::IsNan, bool_type_, {right})});
        condition = (opcode == "VCmpUF32" || opcode == "VCmpxUF32")
            ? unordered
            : module_.AddInstruction(SpirvOp::LogicalNot, bool_type_, {unordered});
    } else if (opcode.size() > 3 &&
               opcode.compare(opcode.size() - 3, 3, "F32") == 0) {
        const u32 left = GetFloatSource(instruction, 0);
        const u32 right = GetFloatSource(instruction, 1);
        SpirvOp operation;
        if (opcode == "VCmpLtF32" || opcode == "VCmpxLtF32") {
            operation = SpirvOp::FOrdLessThan;
        } else if (opcode == "VCmpEqF32" || opcode == "VCmpxEqF32") {
            operation = SpirvOp::FOrdEqual;
        } else if (opcode == "VCmpLeF32" || opcode == "VCmpxLeF32") {
            operation = SpirvOp::FOrdLessThanEqual;
        } else if (opcode == "VCmpGtF32" || opcode == "VCmpxGtF32") {
            operation = SpirvOp::FOrdGreaterThan;
        } else if (opcode == "VCmpLgF32" || opcode == "VCmpxLgF32") {
            operation = SpirvOp::FOrdNotEqual;
        } else if (opcode == "VCmpGeF32" || opcode == "VCmpxGeF32") {
            operation = SpirvOp::FOrdGreaterThanEqual;
        } else if (opcode == "VCmpNeqF32" || opcode == "VCmpxNeqF32") {
            operation = SpirvOp::FUnordNotEqual;
        } else if (opcode == "VCmpNltF32" || opcode == "VCmpxNltF32") {
            operation = SpirvOp::FUnordGreaterThanEqual;
        } else if (opcode == "VCmpNleF32" || opcode == "VCmpxNleF32") {
            operation = SpirvOp::FUnordGreaterThan;
        } else if (opcode == "VCmpNgtF32" || opcode == "VCmpxNgtF32") {
            operation = SpirvOp::FUnordLessThanEqual;
        } else if (opcode == "VCmpNgeF32" || opcode == "VCmpxNgeF32") {
            operation = SpirvOp::FUnordLessThan;
        } else if (opcode == "VCmpNlgF32" || opcode == "VCmpxNlgF32") {
            operation = SpirvOp::FUnordEqual;
        } else {
            error = "unsupported float compare " + opcode;
            return false;
        }
        condition = module_.AddInstruction(operation, bool_type_, {left, right});
    } else {
        u32 left = GetRawSource(instruction, 0);
        u32 right = GetRawSource(instruction, 1);
        const bool is_signed = opcode.size() > 3 &&
            opcode.compare(opcode.size() - 3, 3, "I32") == 0;
        if (is_signed) {
            left = Bitcast(int_type_, left);
            right = Bitcast(int_type_, right);
        }

        SpirvOp operation;
        if (opcode == "VCmpEqI32" || opcode == "VCmpxEqI32" ||
            opcode == "VCmpEqU32" || opcode == "VCmpxEqU32") {
            operation = SpirvOp::IEqual;
        } else if (opcode == "VCmpNeI32" || opcode == "VCmpxNeI32" ||
                   opcode == "VCmpNeU32" || opcode == "VCmpxNeU32") {
            operation = SpirvOp::INotEqual;
        } else if (opcode == "VCmpLtI32" || opcode == "VCmpxLtI32") {
            operation = SpirvOp::SLessThan;
        } else if (opcode == "VCmpLeI32" || opcode == "VCmpxLeI32") {
            operation = SpirvOp::SLessThanEqual;
        } else if (opcode == "VCmpGtI32" || opcode == "VCmpxGtI32") {
            operation = SpirvOp::SGreaterThan;
        } else if (opcode == "VCmpGeI32" || opcode == "VCmpxGeI32") {
            operation = SpirvOp::SGreaterThanEqual;
        } else if (opcode == "VCmpLtU32" || opcode == "VCmpxLtU32") {
            operation = SpirvOp::ULessThan;
        } else if (opcode == "VCmpLeU32" || opcode == "VCmpxLeU32") {
            operation = SpirvOp::ULessThanEqual;
        } else if (opcode == "VCmpGtU32" || opcode == "VCmpxGtU32") {
            operation = SpirvOp::UGreaterThan;
        } else if (opcode == "VCmpGeU32" || opcode == "VCmpxGeU32") {
            operation = SpirvOp::UGreaterThanEqual;
        } else {
            error = "unsupported integer compare " + opcode;
            return false;
        }
        condition = module_.AddInstruction(operation, bool_type_, {left, right});
    }

    // Only EXEC-enabled lanes can pass the test: mask = EXEC & condition.
    const u32 active_condition = module_.AddInstruction(
        SpirvOp::LogicalAnd, bool_type_, {Load(bool_type_, exec_), condition});
    if (opcode.rfind("VCmpx", 0) == 0) {
        // VCMPX is EXEC-only; no SGPR destination.
        StoreWaveMask(126, active_condition);
    } else {
        u32 compare_destination = 106;
        if (const auto* control = std::get_if<GcnSdwaControl>(&instruction.control)) {
            if (control->scalar_destination.has_value()) {
                compare_destination = *control->scalar_destination;
            }
        }
        StoreWaveMask(compare_destination, active_condition);
    }
    return true;
}

} // namespace GPU::Shader

namespace GPU::Shader {

// ---------------------------------------------------------------------------
// Vector ALU (TryEmitVectorAlu).  Corpus-driven opcode set: F32 arithmetic
// and conversion dominate; SDWA/DPP controls fail loudly (absent from the
// corpus) instead of mistranslating.
// ---------------------------------------------------------------------------
bool SpirvTranslateContext::TryEmitVectorAlu(
    const GcnInstruction& instruction, std::string& error) {
    error.clear();
    const std::string& opcode = instruction.opcode;

    if (std::get_if<GcnDppControl>(&instruction.control) ||
        std::get_if<GcnDpp8Control>(&instruction.control)) {
        error = "DPP controls are not supported yet in " + opcode;
        return false;
    }
    if (std::get_if<GcnVop3pControl>(&instruction.control)) {
        error = "packed-f16 (VOP3P) ops are not supported yet in " + opcode;
        return false;
    }

    if (opcode.rfind("VCmp", 0) == 0) {
        return TryEmitVectorCompare(instruction, error);
    }

    if (opcode == "VReadlaneB32" || opcode == "VReadfirstlaneB32") {
        // Single-lane model: the current lane's value is the broadcast.
        if (instruction.destinations.empty() ||
            instruction.destinations[0].kind != GcnOperandKind::ScalarRegister ||
            instruction.sources.empty()) {
            error = "invalid readlane operands";
            return false;
        }
        StoreS(instruction.destinations[0].value, GetRawSource(instruction, 0));
        return true;
    }

    u32 destination = 0;
    if (!TryGetVectorDestination(instruction, destination)) {
        error = "missing vector destination";
        return false;
    }

    u32 result;
    if (opcode == "VMovB32") {
        result = GetRawSource(instruction, 0);
    } else if (opcode == "VCndmaskB32") {
        const u32 condition = instruction.sources.size() > 2
            ? IsWaveMaskActive(GetRawSource64(instruction, 2))
            : Load(bool_type_, vcc_);
        result = module_.AddInstruction(
            SpirvOp::Select, uint_type_,
            {condition, GetRawSource(instruction, 1), GetRawSource(instruction, 0)});
    } else if (opcode == "VCvtU32F32") {
        result = module_.AddInstruction(
            SpirvOp::ConvertFToU, uint_type_, {GetFloatSource(instruction, 0)});
    } else if (opcode == "VCvtI32F32" || opcode == "VCvtRpiI32F32" ||
               opcode == "VCvtFlrI32F32") {
        u32 source = GetFloatSource(instruction, 0);
        if (opcode == "VCvtRpiI32F32") {
            source = Ext(GlslExt::Ceil, float_type_, {source});
        } else if (opcode == "VCvtFlrI32F32") {
            source = Ext(GlslExt::Floor, float_type_, {source});
        }
        result = Bitcast(
            uint_type_,
            module_.AddInstruction(SpirvOp::ConvertFToS, int_type_, {source}));
    } else if (opcode == "VCvtF32I32") {
        const u32 signed_value = Bitcast(int_type_, GetRawSource(instruction, 0));
        result = Bitcast(
            uint_type_,
            module_.AddInstruction(SpirvOp::ConvertSToF, float_type_, {signed_value}));
    } else if (opcode == "VCvtF32U32") {
        result = Bitcast(
            uint_type_,
            module_.AddInstruction(
                SpirvOp::ConvertUToF, float_type_, {GetRawSource(instruction, 0)}));
    } else if (opcode == "VCvtF32Ubyte0" || opcode == "VCvtF32Ubyte1" ||
               opcode == "VCvtF32Ubyte2" || opcode == "VCvtF32Ubyte3") {
        const u32 shift = static_cast<u32>(opcode.back() - '0') * 8;
        u32 raw = ShiftRightLogical(GetRawSource(instruction, 0), UInt(shift));
        raw = BitwiseAnd(raw, UInt(0xFF));
        result = Bitcast(
            uint_type_,
            module_.AddInstruction(SpirvOp::ConvertUToF, float_type_, {raw}));
    } else if (opcode == "VCvtF16F32") {
        const u32 vector = module_.AddInstruction(
            SpirvOp::CompositeConstruct, vec2_type_,
            {GetFloatSource(instruction, 0), Float(0)});
        result = BitwiseAnd(Ext(GlslExt::PackHalf2x16, uint_type_, {vector}),
                            UInt(0xFFFF));
    } else if (opcode == "VCvtF32F16") {
        const u32 unpacked = Ext(
            GlslExt::UnpackHalf2x16, vec2_type_, {GetRawSource(instruction, 0)});
        const u32 value = module_.AddInstruction(
            SpirvOp::CompositeExtract, float_type_, {unpacked, 0});
        result = Bitcast(uint_type_, value);
    } else if (opcode == "VRcpF32" || opcode == "VRcpIflagF32") {
        result = EmitFloatResult(
            instruction,
            module_.AddInstruction(
                SpirvOp::FDiv, float_type_,
                {Float(1), GetFloatSource(instruction, 0)}));
    } else if (opcode == "VLogF32") {
        result = EmitFloatResult(
            instruction,
            Ext(GlslExt::Log2, float_type_, {GetFloatSource(instruction, 0)}));
    } else if (opcode == "VExpF32") {
        result = EmitFloatResult(
            instruction,
            Ext(GlslExt::Exp2, float_type_, {GetFloatSource(instruction, 0)}));
    } else if (opcode == "VRsqF32") {
        result = EmitFloatResult(
            instruction,
            Ext(GlslExt::InverseSqrt, float_type_, {GetFloatSource(instruction, 0)}));
    } else if (opcode == "VFractF32") {
        result = EmitFloatResult(
            instruction,
            Ext(GlslExt::Fract, float_type_, {GetFloatSource(instruction, 0)}));
    } else if (opcode == "VTruncF32") {
        result = EmitFloatResult(
            instruction,
            Ext(GlslExt::Trunc, float_type_, {GetFloatSource(instruction, 0)}));
    } else if (opcode == "VCeilF32") {
        result = EmitFloatResult(
            instruction,
            Ext(GlslExt::Ceil, float_type_, {GetFloatSource(instruction, 0)}));
    } else if (opcode == "VRndneF32") {
        result = EmitFloatResult(
            instruction,
            Ext(GlslExt::RoundEven, float_type_, {GetFloatSource(instruction, 0)}));
    } else if (opcode == "VFloorF32") {
        result = EmitFloatResult(
            instruction,
            Ext(GlslExt::Floor, float_type_, {GetFloatSource(instruction, 0)}));
    } else if (opcode == "VSqrtF32") {
        result = EmitFloatResult(
            instruction,
            Ext(GlslExt::Sqrt, float_type_, {GetFloatSource(instruction, 0)}));
    } else if (opcode == "VSinF32") {
        // GCN trig inputs are in turns; GLSL expects radians.
        result = EmitFloatResult(
            instruction,
            Ext(GlslExt::Sin, float_type_,
                {module_.AddInstruction(
                    SpirvOp::FMul, float_type_,
                    {GetFloatSource(instruction, 0), Float(6.2831855f)})}));
    } else if (opcode == "VCosF32") {
        result = EmitFloatResult(
            instruction,
            Ext(GlslExt::Cos, float_type_,
                {module_.AddInstruction(
                    SpirvOp::FMul, float_type_,
                    {GetFloatSource(instruction, 0), Float(6.2831855f)})}));
    } else if (opcode == "VAddF32") {
        result = EmitFloatBinary(instruction, SpirvOp::FAdd);
    } else if (opcode == "VSubF32") {
        result = EmitFloatBinary(instruction, SpirvOp::FSub);
    } else if (opcode == "VSubrevF32") {
        result = EmitFloatBinary(instruction, SpirvOp::FSub, /*reverse=*/true);
    } else if (opcode == "VMulF32") {
        result = EmitFloatBinary(instruction, SpirvOp::FMul);
    } else if (opcode == "VMinF32") {
        result = EmitFloatExtBinary(instruction, GlslExt::FMin);
    } else if (opcode == "VMaxF32") {
        result = EmitFloatExtBinary(instruction, GlslExt::FMax);
    } else if (opcode == "VMadF32" || opcode == "VFmaF32" ||
               opcode == "VMadMkF32" || opcode == "VMadAkF32" ||
               opcode == "VFmaMkF32" || opcode == "VFmaAkF32") {
        result = EmitFloatResult(
            instruction,
            Ext(GlslExt::Fma, float_type_,
                {GetFloatSource(instruction, 0), GetFloatSource(instruction, 1),
                 GetFloatSource(instruction, 2)}));
    } else if (opcode == "VMacF32" || opcode == "VFmacF32") {
        const u32 addend = Bitcast(float_type_, LoadV(destination));
        result = EmitFloatResult(
            instruction,
            Ext(GlslExt::Fma, float_type_,
                {GetFloatSource(instruction, 0), GetFloatSource(instruction, 1),
                 addend}));
    } else if (opcode == "VMin3F32") {
        result = EmitFloatTernaryExt(instruction, GlslExt::FMin);
    } else if (opcode == "VMax3F32") {
        result = EmitFloatTernaryExt(instruction, GlslExt::FMax);
    } else if (opcode == "VMed3F32") {
        const u32 left = GetFloatSource(instruction, 0);
        const u32 middle = GetFloatSource(instruction, 1);
        const u32 right = GetFloatSource(instruction, 2);
        const u32 low = Ext(GlslExt::FMin, float_type_, {left, middle});
        const u32 high = Ext(GlslExt::FMax, float_type_, {left, middle});
        result = EmitFloatResult(
            instruction,
            Ext(GlslExt::FMax, float_type_,
                {low, Ext(GlslExt::FMin, float_type_, {high, right})}));
    } else if (opcode == "VAndB32") {
        result = EmitIntegerBinary(instruction, SpirvOp::BitwiseAnd);
    } else if (opcode == "VOrB32") {
        result = EmitIntegerBinary(instruction, SpirvOp::BitwiseOr);
    } else if (opcode == "VXorB32") {
        result = EmitIntegerBinary(instruction, SpirvOp::BitwiseXor);
    } else if (opcode == "VXnorB32") {
        result = module_.AddInstruction(
            SpirvOp::Not, uint_type_,
            {EmitIntegerBinary(instruction, SpirvOp::BitwiseXor)});
    } else if (opcode == "VNotB32") {
        result = module_.AddInstruction(
            SpirvOp::Not, uint_type_, {GetRawSource(instruction, 0)});
    } else if (opcode == "VBfrevB32") {
        result = module_.AddInstruction(
            SpirvOp::BitReverse, uint_type_, {GetRawSource(instruction, 0)});
    } else if (opcode == "VAddI32" || opcode == "VAddU32") {
        result = EmitIntegerBinary(instruction, SpirvOp::IAdd);
    } else if (opcode == "VAddcU32" || opcode == "VAddCoCiU32") {
        result = EmitAddWithCarry(instruction);
    } else if (opcode == "VSubI32" || opcode == "VSubU32") {
        result = EmitIntegerBinary(instruction, SpirvOp::ISub);
    } else if (opcode == "VSubrevI32" || opcode == "VSubrevU32") {
        result = EmitIntegerBinary(instruction, SpirvOp::ISub, /*reverse=*/true);
    } else if (opcode == "VSubbU32") {
        result = EmitSubtractWithBorrow(instruction, /*reverse=*/false);
    } else if (opcode == "VSubbrevU32") {
        result = EmitSubtractWithBorrow(instruction, /*reverse=*/true);
    } else if (opcode == "VMulLoU32" || opcode == "VMulLoI32") {
        result = EmitIntegerBinary(instruction, SpirvOp::IMul);
    } else if (opcode == "VMulHiU32") {
        const u32 left64 = module_.AddInstruction(
            SpirvOp::UConvert, ulong_type_, {GetRawSource(instruction, 0)});
        const u32 right64 = module_.AddInstruction(
            SpirvOp::UConvert, ulong_type_, {GetRawSource(instruction, 1)});
        const u32 product =
            module_.AddInstruction(SpirvOp::IMul, ulong_type_, {left64, right64});
        result = module_.AddInstruction(
            SpirvOp::UConvert, uint_type_,
            {ShiftRightLogical64(product, module_.Constant64(ulong_type_, 32))});
    } else if (opcode == "VLshrB32") {
        result = EmitIntegerBinary(instruction, SpirvOp::ShiftRightLogical);
    } else if (opcode == "VLshrrevB32") {
        result = EmitIntegerBinary(
            instruction, SpirvOp::ShiftRightLogical, /*reverse=*/true);
    } else if (opcode == "VLshlB32") {
        result = EmitIntegerBinary(instruction, SpirvOp::ShiftLeftLogical);
    } else if (opcode == "VLshlrevB32") {
        result = EmitIntegerBinary(
            instruction, SpirvOp::ShiftLeftLogical, /*reverse=*/true);
    } else if (opcode == "VAshrI32" || opcode == "VAshrrevI32") {
        const bool reverse = opcode == "VAshrrevI32";
        const u32 left = GetRawSource(instruction, reverse ? 1 : 0);
        const u32 right = GetRawSource(instruction, reverse ? 0 : 1);
        result = ShiftRightArithmetic(left, right);
    } else if (opcode == "VBfeU32") {
        result = module_.AddInstruction(
            SpirvOp::BitFieldUExtract, uint_type_,
            {GetRawSource(instruction, 0),
             BitwiseAnd(GetRawSource(instruction, 1), UInt(31)),
             BitwiseAnd(GetRawSource(instruction, 2), UInt(31))});
    } else if (opcode == "VBfiB32") {
        const u32 mask = GetRawSource(instruction, 0);
        const u32 insert = GetRawSource(instruction, 1);
        const u32 source = GetRawSource(instruction, 2);
        result = module_.AddInstruction(
            SpirvOp::BitwiseOr, uint_type_,
            {BitwiseAnd(mask, insert),
             BitwiseAnd(
                 module_.AddInstruction(SpirvOp::Not, uint_type_, {mask}), source)});
    } else if (opcode == "VCvtPkrtzF16F32") {
        const u32 first = TruncateFloat32ForPack(GetFloatSource(instruction, 0));
        const u32 second = TruncateFloat32ForPack(GetFloatSource(instruction, 1));
        const u32 vector = module_.AddInstruction(
            SpirvOp::CompositeConstruct, vec2_type_, {first, second});
        result = Ext(GlslExt::PackHalf2x16, uint_type_, {vector});
    } else if (opcode == "VCvtPknormI16F32" || opcode == "VCvtPknormU16F32") {
        const u32 vector = module_.AddInstruction(
            SpirvOp::CompositeConstruct, vec2_type_,
            {GetFloatSource(instruction, 0), GetFloatSource(instruction, 1)});
        // GLSL.std.450 56=PackSnorm2x16, 57=PackUnorm2x16.
        result = Ext(
            opcode == "VCvtPknormI16F32" ? 56u : 57u, uint_type_, {vector});
    } else {
        error = "unsupported vector opcode " + opcode;
        return false;
    }

    if (const auto* sdwa = std::get_if<GcnSdwaControl>(&instruction.control)) {
        if (!sdwa->scalar_destination.has_value()) {
            result = ApplySdwaDestination(*sdwa, result, LoadV(destination));
        }
    }

    StoreV(destination, result);
    return true;
}

} // namespace GPU::Shader

namespace GPU::Shader {

// ---------------------------------------------------------------------------
// Scalar compares (SOPC, and SOPK immediate compares).
// ---------------------------------------------------------------------------
bool SpirvTranslateContext::TryEmitScalarCompare(
    const GcnInstruction& instruction, std::string& error) {
    if (instruction.sources.size() < 2) {
        error = "missing scalar compare source";
        return false;
    }

    u32 left = GetRawSource(instruction, 0);
    u32 right = GetRawSource(instruction, 1);
    const std::string& opcode = instruction.opcode;
    if (opcode == "SBitcmp0B32" || opcode == "SBitcmp1B32") {
        const u32 shifted = ShiftRightLogical(left, BitwiseAnd(right, UInt(31)));
        const u32 is_set = IsNotZero(BitwiseAnd(shifted, UInt(1)));
        Store(scc_,
              opcode == "SBitcmp1B32"
                  ? is_set
                  : module_.AddInstruction(SpirvOp::LogicalNot, bool_type_, {is_set}));
        return true;
    }

    SpirvOp operation;
    if (opcode == "SCmpEqI32" || opcode == "SCmpEqU32") {
        operation = SpirvOp::IEqual;
    } else if (opcode == "SCmpLgI32" || opcode == "SCmpLgU32") {
        operation = SpirvOp::INotEqual;
    } else if (opcode == "SCmpGtI32") {
        operation = SpirvOp::SGreaterThan;
    } else if (opcode == "SCmpGeI32") {
        operation = SpirvOp::SGreaterThanEqual;
    } else if (opcode == "SCmpLtI32") {
        operation = SpirvOp::SLessThan;
    } else if (opcode == "SCmpLeI32") {
        operation = SpirvOp::SLessThanEqual;
    } else if (opcode == "SCmpGtU32") {
        operation = SpirvOp::UGreaterThan;
    } else if (opcode == "SCmpGeU32") {
        operation = SpirvOp::UGreaterThanEqual;
    } else if (opcode == "SCmpLtU32") {
        operation = SpirvOp::ULessThan;
    } else if (opcode == "SCmpLeU32") {
        operation = SpirvOp::ULessThanEqual;
    } else {
        error = "unsupported scalar compare " + opcode;
        return false;
    }

    if (opcode.size() > 3 && opcode.compare(opcode.size() - 3, 3, "I32") == 0) {
        left = Bitcast(int_type_, left);
        right = Bitcast(int_type_, right);
    }
    Store(scc_, module_.AddInstruction(operation, bool_type_, {left, right}));
    return true;
}

// ---------------------------------------------------------------------------
// 64-bit scalar ops (TryEmitScalar64).
// ---------------------------------------------------------------------------
bool SpirvTranslateContext::TryEmitScalar64(
    const GcnInstruction& instruction, u32 destination, std::string& error) {
    const std::string& opcode = instruction.opcode;
    const u32 left = GetRawSource64(instruction, 0);

    if (opcode.size() > 11 &&
        opcode.compare(opcode.size() - 11, 11, "SaveexecB64") == 0) {
        const u32 old_exec = BooleanToWaveMask(Load(bool_type_, exec_));
        const u32 not_left =
            module_.AddInstruction(SpirvOp::Not, ulong_type_, {left});
        const u32 not_old_exec =
            module_.AddInstruction(SpirvOp::Not, ulong_type_, {old_exec});
        u32 new_exec = 0;
        if (opcode == "SAndSaveexecB64") {
            new_exec = module_.AddInstruction(
                SpirvOp::BitwiseAnd, ulong_type_, {old_exec, left});
        } else if (opcode == "SOrSaveexecB64") {
            new_exec = module_.AddInstruction(
                SpirvOp::BitwiseOr, ulong_type_, {old_exec, left});
        } else if (opcode == "SXorSaveexecB64") {
            new_exec = module_.AddInstruction(
                SpirvOp::BitwiseXor, ulong_type_, {old_exec, left});
        } else if (opcode == "SAndn2SaveexecB64") {
            new_exec = module_.AddInstruction(
                SpirvOp::BitwiseAnd, ulong_type_, {left, not_old_exec});
        } else if (opcode == "SAndn1SaveexecB64") {
            new_exec = module_.AddInstruction(
                SpirvOp::BitwiseAnd, ulong_type_, {not_left, old_exec});
        } else if (opcode == "SOrn1SaveexecB64") {
            new_exec = module_.AddInstruction(
                SpirvOp::BitwiseOr, ulong_type_, {not_left, old_exec});
        } else if (opcode == "SOrn2SaveexecB64") {
            new_exec = module_.AddInstruction(
                SpirvOp::BitwiseOr, ulong_type_, {left, not_old_exec});
        } else if (opcode == "SNandSaveexecB64") {
            new_exec = module_.AddInstruction(
                SpirvOp::Not, ulong_type_,
                {module_.AddInstruction(
                    SpirvOp::BitwiseAnd, ulong_type_, {left, old_exec})});
        } else if (opcode == "SNorSaveexecB64") {
            new_exec = module_.AddInstruction(
                SpirvOp::Not, ulong_type_,
                {module_.AddInstruction(
                    SpirvOp::BitwiseOr, ulong_type_, {left, old_exec})});
        } else if (opcode == "SXnorSaveexecB64") {
            new_exec = module_.AddInstruction(
                SpirvOp::Not, ulong_type_,
                {module_.AddInstruction(
                    SpirvOp::BitwiseXor, ulong_type_, {left, old_exec})});
        }
        if (new_exec == 0) {
            error = "unsupported scalar 64-bit saveexec opcode " + opcode;
            return false;
        }

        StoreS64(destination, old_exec);
        StoreS64(126, new_exec);
        Store(scc_, IsNotZero64(new_exec));
        return true;
    }

    if (opcode == "SLshlB64" || opcode == "SLshrB64") {
        if (instruction.sources.size() < 2) {
            error = "missing scalar 64-bit shift source";
            return false;
        }
        const u32 shift = module_.AddInstruction(
            SpirvOp::UConvert, ulong_type_, {GetRawSource(instruction, 1)});
        const u32 shifted = opcode == "SLshlB64"
            ? ShiftLeftLogical64(left, shift)
            : ShiftRightLogical64(left, shift);
        StoreS64(destination, shifted);
        Store(scc_, IsNotZero64(shifted));
        return true;
    }

    if (opcode == "SBfeU64" || opcode == "SBfeI64") {
        if (instruction.sources.size() < 2) {
            error = "missing scalar 64-bit bitfield source";
            return false;
        }
        const u32 control = GetRawSource(instruction, 1);
        const u32 offset = BitwiseAnd(control, UInt(63));
        const u32 requested_width =
            BitwiseAnd(ShiftRightLogical(control, UInt(16)), UInt(0x7F));
        const u32 remaining = module_.AddInstruction(
            SpirvOp::ISub, uint_type_, {UInt(64), offset});
        const u32 width =
            Ext(GlslExt::UMin, uint_type_, {requested_width, remaining});
        const u32 offset64 =
            module_.AddInstruction(SpirvOp::UConvert, ulong_type_, {offset});
        const u32 width64 =
            module_.AddInstruction(SpirvOp::UConvert, ulong_type_, {width});
        const u32 one64 = module_.Constant64(ulong_type_, 1);
        const u32 shifted = ShiftRightLogical64(left, offset64);
        const u32 partial_mask = module_.AddInstruction(
            SpirvOp::ISub, ulong_type_,
            {ShiftLeftLogical64(one64, width64), one64});
        const u32 full_width = module_.AddInstruction(
            SpirvOp::IEqual, bool_type_, {width, UInt(64)});
        const u32 mask = module_.AddInstruction(
            SpirvOp::Select, ulong_type_,
            {full_width, module_.Constant64(ulong_type_, 0xFFFFFFFFFFFFFFFFull),
             partial_mask});
        u32 extracted =
            module_.AddInstruction(SpirvOp::BitwiseAnd, ulong_type_, {shifted, mask});
        if (opcode == "SBfeI64") {
            const u32 sign_shift = module_.AddInstruction(
                SpirvOp::ISub, uint_type_, {width, UInt(1)});
            const u32 sign_bit = ShiftLeftLogical64(
                one64,
                module_.AddInstruction(SpirvOp::UConvert, ulong_type_, {sign_shift}));
            const u32 sign_extended = module_.AddInstruction(
                SpirvOp::ISub, ulong_type_,
                {module_.AddInstruction(
                     SpirvOp::BitwiseXor, ulong_type_, {extracted, sign_bit}),
                 sign_bit});
            extracted = module_.AddInstruction(
                SpirvOp::Select, ulong_type_,
                {module_.AddInstruction(SpirvOp::IEqual, bool_type_, {width, UInt(0)}),
                 module_.Constant64(ulong_type_, 0), sign_extended});
        }
        StoreS64(destination, extracted);
        Store(scc_, IsNotZero64(extracted));
        return true;
    }

    u32 value = 0;
    bool set_scc = false;
    if (opcode == "SMovB64") {
        value = left;
    } else if (opcode == "SWqmB64") {
        // Quad-wide mask: or-reduce each quad of bits, then expand 0x1111->0xF.
        u32 quad_any = module_.AddInstruction(
            SpirvOp::BitwiseOr, ulong_type_,
            {left,
             module_.AddInstruction(
                 SpirvOp::BitwiseOr, ulong_type_,
                 {ShiftRightLogical64(left, module_.Constant64(ulong_type_, 1)),
                  module_.AddInstruction(
                      SpirvOp::BitwiseOr, ulong_type_,
                      {ShiftRightLogical64(left, module_.Constant64(ulong_type_, 2)),
                       ShiftRightLogical64(
                           left, module_.Constant64(ulong_type_, 3))})})});
        quad_any = module_.AddInstruction(
            SpirvOp::BitwiseAnd, ulong_type_,
            {quad_any, module_.Constant64(ulong_type_, 0x1111111111111111ull)});
        value = module_.AddInstruction(
            SpirvOp::IMul, ulong_type_,
            {quad_any, module_.Constant64(ulong_type_, 0xFull)});
    } else if (opcode == "SNotB64") {
        value = module_.AddInstruction(SpirvOp::Not, ulong_type_, {left});
        set_scc = true;
    } else {
        if (instruction.sources.size() < 2) {
            error = "missing scalar 64-bit source";
            return false;
        }
        const u32 right = GetRawSource64(instruction, 1);
        if (opcode == "SAndB64") {
            value = module_.AddInstruction(SpirvOp::BitwiseAnd, ulong_type_, {left, right});
            set_scc = true;
        } else if (opcode == "SOrB64") {
            value = module_.AddInstruction(SpirvOp::BitwiseOr, ulong_type_, {left, right});
            set_scc = true;
        } else if (opcode == "SXorB64") {
            value = module_.AddInstruction(SpirvOp::BitwiseXor, ulong_type_, {left, right});
            set_scc = true;
        } else if (opcode == "SNandB64") {
            value = module_.AddInstruction(
                SpirvOp::Not, ulong_type_,
                {module_.AddInstruction(SpirvOp::BitwiseAnd, ulong_type_, {left, right})});
            set_scc = true;
        } else if (opcode == "SNorB64") {
            value = module_.AddInstruction(
                SpirvOp::Not, ulong_type_,
                {module_.AddInstruction(SpirvOp::BitwiseOr, ulong_type_, {left, right})});
            set_scc = true;
        } else if (opcode == "SXnorB64") {
            value = module_.AddInstruction(
                SpirvOp::Not, ulong_type_,
                {module_.AddInstruction(SpirvOp::BitwiseXor, ulong_type_, {left, right})});
            set_scc = true;
        } else if (opcode == "SAndn1B64") {
            value = module_.AddInstruction(
                SpirvOp::BitwiseAnd, ulong_type_,
                {module_.AddInstruction(SpirvOp::Not, ulong_type_, {left}), right});
            set_scc = true;
        } else if (opcode == "SAndn2B64") {
            value = module_.AddInstruction(
                SpirvOp::BitwiseAnd, ulong_type_,
                {left, module_.AddInstruction(SpirvOp::Not, ulong_type_, {right})});
            set_scc = true;
        } else if (opcode == "SOrn1B64") {
            value = module_.AddInstruction(
                SpirvOp::BitwiseOr, ulong_type_,
                {module_.AddInstruction(SpirvOp::Not, ulong_type_, {left}), right});
            set_scc = true;
        } else if (opcode == "SOrn2B64") {
            value = module_.AddInstruction(
                SpirvOp::BitwiseOr, ulong_type_,
                {left, module_.AddInstruction(SpirvOp::Not, ulong_type_, {right})});
            set_scc = true;
        } else if (opcode == "SCselectB64") {
            value = module_.AddInstruction(
                SpirvOp::Select, ulong_type_,
                {Load(bool_type_, scc_), left, right});
        } else {
            error = "unsupported scalar 64-bit opcode " + opcode;
            return false;
        }
    }

    StoreS64(destination, value);
    if (set_scc) {
        Store(scc_, IsNotZero64(value));
    }
    return true;
}

} // namespace GPU::Shader

namespace GPU::Shader {

// ---------------------------------------------------------------------------
// Scalar ALU (TryEmitScalarAlu): SOPC compares, SOPK immediates, B64 ops,
// saveexec, and the 32-bit arithmetic/logic set.
// ---------------------------------------------------------------------------
bool SpirvTranslateContext::TryEmitScalarAlu(
    const GcnInstruction& instruction, std::string& error) {
    error.clear();
    const std::string& opcode = instruction.opcode;
    if (instruction.encoding == GcnEncoding::Sopc) {
        return TryEmitScalarCompare(instruction, error);
    }

    if (instruction.destinations.empty() ||
        instruction.destinations[0].kind != GcnOperandKind::ScalarRegister) {
        error = "missing scalar destination";
        return false;
    }
    const u32 destination = instruction.destinations[0].value;

    if (instruction.encoding == GcnEncoding::Sopk) {
        const u32 immediate =
            static_cast<u32>(static_cast<s16>(instruction.words[0] & 0xFFFF));
        if (opcode.rfind("SCmpk", 0) == 0) {
            // SCmpk*: compare the destination register against simm16.
            u32 left = LoadS(destination);
            u32 right = UInt(immediate);
            SpirvOp operation;
            if (opcode == "SCmpkEqI32" || opcode == "SCmpkEqU32") {
                operation = SpirvOp::IEqual;
            } else if (opcode == "SCmpkLgI32" || opcode == "SCmpkLgU32") {
                operation = SpirvOp::INotEqual;
            } else if (opcode == "SCmpkGtI32") {
                operation = SpirvOp::SGreaterThan;
            } else if (opcode == "SCmpkGeI32") {
                operation = SpirvOp::SGreaterThanEqual;
            } else if (opcode == "SCmpkLtI32") {
                operation = SpirvOp::SLessThan;
            } else if (opcode == "SCmpkLeI32") {
                operation = SpirvOp::SLessThanEqual;
            } else if (opcode == "SCmpkGtU32") {
                operation = SpirvOp::UGreaterThan;
            } else if (opcode == "SCmpkGeU32") {
                operation = SpirvOp::UGreaterThanEqual;
            } else if (opcode == "SCmpkLtU32") {
                operation = SpirvOp::ULessThan;
            } else if (opcode == "SCmpkLeU32") {
                operation = SpirvOp::ULessThanEqual;
            } else {
                error = "unsupported scalar immediate compare " + opcode;
                return false;
            }
            if (opcode.size() > 3 && opcode.compare(opcode.size() - 3, 3, "I32") == 0) {
                left = Bitcast(int_type_, left);
                right = Bitcast(int_type_, right);
            }
            Store(scc_, module_.AddInstruction(operation, bool_type_, {left, right}));
            return true;
        }

        const u32 current = LoadS(destination);
        u32 value;
        if (opcode == "SMovkI32") {
            value = UInt(immediate);
        } else if (opcode == "SAddkI32") {
            value = IAdd(current, UInt(immediate));
        } else if (opcode == "SMulkI32") {
            value = module_.AddInstruction(
                SpirvOp::IMul, uint_type_, {current, UInt(immediate)});
        } else {
            error = "unsupported scalar immediate " + opcode;
            return false;
        }
        StoreS(destination, value);
        return true;
    }

    if (opcode == "SGetpcB64") {
        // Guest code base is not tracked by the translator; the PC value is
        // only used for address arithmetic against runtime state.
        const u64 pc = instruction.pc +
            static_cast<u64>(instruction.words.size()) * sizeof(u32);
        StoreS(destination, UInt(static_cast<u32>(pc)));
        StoreS(destination + 1, UInt(static_cast<u32>(pc >> 32)));
        return true;
    }

    if ((opcode.size() > 3 && opcode.compare(opcode.size() - 3, 3, "B64") == 0) ||
        opcode == "SWqmB64" || opcode == "SBfeU64" || opcode == "SBfeI64") {
        return TryEmitScalar64(instruction, destination, error);
    }

    const u32 left = GetRawSource(instruction, 0);
    if (opcode.size() > 11 &&
        opcode.compare(opcode.size() - 11, 11, "SaveexecB32") == 0) {
        const u32 old_exec64 = BooleanToWaveMask(Load(bool_type_, exec_));
        const u32 old_exec =
            module_.AddInstruction(SpirvOp::UConvert, uint_type_, {old_exec64});
        const u32 not_left = module_.AddInstruction(SpirvOp::Not, uint_type_, {left});
        const u32 not_old_exec =
            module_.AddInstruction(SpirvOp::Not, uint_type_, {old_exec});
        u32 new_exec = 0;
        if (opcode == "SAndSaveexecB32") {
            new_exec = BitwiseAnd(old_exec, left);
        } else if (opcode == "SOrSaveexecB32") {
            new_exec = BitwiseOr(old_exec, left);
        } else if (opcode == "SXorSaveexecB32") {
            new_exec = BitwiseXor(old_exec, left);
        } else if (opcode == "SAndn1SaveexecB32") {
            new_exec = BitwiseAnd(not_left, old_exec);
        } else if (opcode == "SAndn2SaveexecB32") {
            new_exec = BitwiseAnd(left, not_old_exec);
        } else if (opcode == "SOrn1SaveexecB32") {
            new_exec = BitwiseOr(not_left, old_exec);
        } else if (opcode == "SOrn2SaveexecB32") {
            new_exec = BitwiseOr(left, not_old_exec);
        } else if (opcode == "SNandSaveexecB32") {
            new_exec = module_.AddInstruction(
                SpirvOp::Not, uint_type_, {BitwiseAnd(left, old_exec)});
        } else if (opcode == "SNorSaveexecB32") {
            new_exec = module_.AddInstruction(
                SpirvOp::Not, uint_type_, {BitwiseOr(left, old_exec)});
        } else if (opcode == "SXnorSaveexecB32") {
            new_exec = module_.AddInstruction(
                SpirvOp::Not, uint_type_, {BitwiseXor(left, old_exec)});
        } else {
            error = "unsupported scalar 32-bit saveexec opcode " + opcode;
            return false;
        }

        StoreS(destination, old_exec);
        // B32 saveexec is the wave32 form; EXEC_HI is zero.
        const u32 new_exec64 =
            module_.AddInstruction(SpirvOp::UConvert, ulong_type_, {new_exec});
        StoreS64(126, new_exec64);
        Store(scc_, IsNotZero(new_exec));
        return true;
    }

    u32 result;
    if (opcode == "SMovB32") {
        result = left;
        StoreS(destination, result);
        return true;
    }
    if (opcode == "SNotB32") {
        result = module_.AddInstruction(SpirvOp::Not, uint_type_, {left});
        StoreS(destination, result);
        Store(scc_, IsNotZero(result));
        return true;
    }
    if (opcode == "SBrevB32") {
        result = module_.AddInstruction(SpirvOp::BitReverse, uint_type_, {left});
        StoreS(destination, result);
        Store(scc_, IsNotZero(result));
        return true;
    }
    if (opcode == "SBcnt1I32B32") {
        result = module_.AddInstruction(SpirvOp::BitCount, uint_type_, {left});
        StoreS(destination, result);
        Store(scc_, IsNotZero(result));
        return true;
    }
    if (opcode == "SFF1I32B32") {
        result = Ext(GlslExt::FindILsb, uint_type_, {left});
        StoreS(destination, result);
        Store(scc_, IsNotZero(result));
        return true;
    }
    if (opcode == "SBitset1B32") {
        result = module_.AddInstruction(
            SpirvOp::BitFieldInsert, uint_type_,
            {LoadS(destination), UInt(1), BitwiseAnd(left, UInt(31)), UInt(1)});
        StoreS(destination, result);
        return true;
    }

    if (instruction.sources.size() < 2) {
        error = "missing scalar source for " + opcode;
        return false;
    }
    const u32 right = GetRawSource(instruction, 1);

    if (opcode == "SAddU32") {
        result = IAdd(left, right);
        Store(scc_,
              module_.AddInstruction(SpirvOp::ULessThan, bool_type_, {result, left}));
    } else if (opcode == "SSubU32") {
        result = module_.AddInstruction(SpirvOp::ISub, uint_type_, {left, right});
        Store(scc_,
              module_.AddInstruction(SpirvOp::UGreaterThan, bool_type_, {right, left}));
    } else if (opcode == "SAddI32") {
        result = IAdd(left, right);
        Store(scc_, SignedAddOverflow(left, right, result));
    } else if (opcode == "SSubI32") {
        result = module_.AddInstruction(SpirvOp::ISub, uint_type_, {left, right});
        Store(scc_, SignedSubOverflow(left, right, result));
    } else if (opcode == "SMulI32") {
        result = module_.AddInstruction(SpirvOp::IMul, uint_type_, {left, right});
    } else if (opcode == "SAndB32") {
        result = BitwiseAnd(left, right);
        Store(scc_, IsNotZero(result));
    } else if (opcode == "SOrB32") {
        result = BitwiseOr(left, right);
        Store(scc_, IsNotZero(result));
    } else if (opcode == "SXorB32") {
        result = BitwiseXor(left, right);
        Store(scc_, IsNotZero(result));
    } else if (opcode == "SAndn2B32") {
        result = BitwiseAnd(
            left, module_.AddInstruction(SpirvOp::Not, uint_type_, {right}));
        Store(scc_, IsNotZero(result));
    } else if (opcode == "SOrn2B32") {
        result = BitwiseOr(
            left, module_.AddInstruction(SpirvOp::Not, uint_type_, {right}));
        Store(scc_, IsNotZero(result));
    } else if (opcode == "SNandB32") {
        result = module_.AddInstruction(
            SpirvOp::Not, uint_type_, {BitwiseAnd(left, right)});
        Store(scc_, IsNotZero(result));
    } else if (opcode == "SNorB32") {
        result = module_.AddInstruction(
            SpirvOp::Not, uint_type_, {BitwiseOr(left, right)});
        Store(scc_, IsNotZero(result));
    } else if (opcode == "SXnorB32") {
        result = module_.AddInstruction(
            SpirvOp::Not, uint_type_, {BitwiseXor(left, right)});
        Store(scc_, IsNotZero(result));
    } else if (opcode == "SLshlB32") {
        result = ShiftLeftLogical(left, right);
        Store(scc_, IsNotZero(result));
    } else if (opcode == "SLshrB32") {
        result = ShiftRightLogical(left, BitwiseAnd(right, UInt(31)));
        Store(scc_, IsNotZero(result));
    } else if (opcode == "SAshrI32") {
        result = ShiftRightArithmetic(left, right);
        Store(scc_, IsNotZero(result));
    } else if (opcode == "SBfmB32") {
        result = module_.AddInstruction(
            SpirvOp::BitFieldInsert, uint_type_,
            {UInt(0), UInt(0xFFFFFFFFu), BitwiseAnd(right, UInt(31)),
             BitwiseAnd(left, UInt(31))});
    } else if (opcode == "SBfeU32" || opcode == "SBfeI32") {
        const u32 offset = BitwiseAnd(right, UInt(31));
        const u32 requested_width =
            BitwiseAnd(ShiftRightLogical(right, UInt(16)), UInt(0x7F));
        const u32 remaining = module_.AddInstruction(
            SpirvOp::ISub, uint_type_, {UInt(32), offset});
        const u32 width = Ext(GlslExt::UMin, uint_type_, {requested_width, remaining});
        result = opcode == "SBfeI32"
            ? Bitcast(
                  uint_type_,
                  module_.AddInstruction(
                      SpirvOp::BitFieldSExtract, int_type_,
                      {Bitcast(int_type_, left), offset, width}))
            : module_.AddInstruction(
                  SpirvOp::BitFieldUExtract, uint_type_, {left, offset, width});
        Store(scc_, IsNotZero(result));
    } else if (opcode == "SCselectB32") {
        result = module_.AddInstruction(
            SpirvOp::Select, uint_type_, {Load(bool_type_, scc_), left, right});
    } else if (opcode == "SMinU32") {
        result = Ext(GlslExt::UMin, uint_type_, {left, right});
        Store(scc_,
              module_.AddInstruction(SpirvOp::ULessThan, bool_type_, {left, right}));
    } else if (opcode == "SMinI32") {
        result = Bitcast(
            uint_type_,
            Ext(GlslExt::SMin, int_type_,
                {Bitcast(int_type_, left), Bitcast(int_type_, right)}));
        Store(scc_,
              module_.AddInstruction(
                  SpirvOp::SLessThan, bool_type_,
                  {Bitcast(int_type_, left), Bitcast(int_type_, right)}));
    } else if (opcode == "SMaxU32") {
        result = Ext(GlslExt::UMax, uint_type_, {left, right});
        Store(scc_,
              module_.AddInstruction(SpirvOp::UGreaterThan, bool_type_, {left, right}));
    } else if (opcode == "SMaxI32") {
        result = Bitcast(
            uint_type_,
            Ext(GlslExt::SMax, int_type_,
                {Bitcast(int_type_, left), Bitcast(int_type_, right)}));
        Store(scc_,
              module_.AddInstruction(
                  SpirvOp::SGreaterThan, bool_type_,
                  {Bitcast(int_type_, left), Bitcast(int_type_, right)}));
    } else if (opcode == "SLshl1AddU32" || opcode == "SLshl2AddU32" ||
               opcode == "SLshl3AddU32" || opcode == "SLshl4AddU32") {
        const u32 shift = static_cast<u32>(opcode[5] - '0');
        result = IAdd(ShiftLeftLogical(left, UInt(shift)), right);
    } else if (opcode == "SPackLlB32B16") {
        result = BitwiseOr(
            BitwiseAnd(left, UInt(0xFFFF)), ShiftLeftLogical(right, UInt(16)));
    } else if (opcode == "SPackLhB32B16") {
        result = BitwiseOr(
            BitwiseAnd(left, UInt(0xFFFF)), BitwiseAnd(right, UInt(0xFFFF0000u)));
    } else if (opcode == "SPackHhB32B16") {
        result = BitwiseOr(
            ShiftRightLogical(left, UInt(16)), BitwiseAnd(right, UInt(0xFFFF0000u)));
    } else {
        error = "unsupported scalar opcode " + opcode;
        return false;
    }

    StoreS(destination, result);
    return true;
}

} // namespace GPU::Shader
