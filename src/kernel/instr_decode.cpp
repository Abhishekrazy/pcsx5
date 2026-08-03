#include "instr_decode.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace InstrDecode {
namespace {

// ---------------------------------------------------------------------------
// Register names — 8/16/32/64-bit families
// ---------------------------------------------------------------------------
const char* kReg8 (int r) { static const char* t[]={"al","cl","dl","bl","spl","bpl","sil","dil","r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b"}; return (r<16)?t[r]:"?"; }
const char* kReg16(int r) { static const char* t[]={"ax","cx","dx","bx","sp","bp","si","di","r8w","r9w","r10w","r11w","r12w","r13w","r14w","r15w"}; return (r<16)?t[r]:"?"; }
const char* kReg32(int r) { static const char* t[]={"eax","ecx","edx","ebx","esp","ebp","esi","edi","r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"}; return (r<16)?t[r]:"?"; }
const char* kReg64(int r) { static const char* t[]={"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15"}; return (r<16)?t[r]:"?"; }
const char* kXmm(int r) { static char b[8]; std::snprintf(b,sizeof(b),"xmm%d",r&15); return b; }

// ---------------------------------------------------------------------------
// Decoder state
// ---------------------------------------------------------------------------
struct State {
    const u8* start;  // base of the buffer
    const u8* pos;    // current read position
    const u8* end;    // one past the last readable byte
    u32       len;    // bytes consumed so far (shadow of pos-start, capped)

    // Prefix flags
    bool rex_w = false, rex_r = false, rex_x = false, rex_b = false;
    bool has_rex = false;
    bool opsize_66 = false; // operand-size override
    bool addr_66  = false;  // 0x67 address-size override (not same as opsize!)
    bool repne    = false;  // F2
    bool rep      = false;  // F3
    bool seg_fs   = false;  // 64
    bool seg_gs   = false;  // 65
    bool lock     = false;  // F0

    u8 NextByte() {
        if (pos >= end) return 0;
        u8 b = *pos++;
        if (len < 15) len++;
        return b;
    }

    s8 NextS8()  { return static_cast<s8>(NextByte()); }
    u32 NextU32(){ u8 a=NextByte(),b=NextByte(),c=NextByte(),d=NextByte(); return (d<<24)|(c<<16)|(b<<8)|a; }

    bool Ok() const { return pos <= end; }

    std::string Reg8 (int r) const { return kReg8 ((r&7)|((rex_b && !(r&8))?8:0)|((r&8)?(has_rex?8:4):0)); }
    std::string Reg16(int r) const { return kReg16((r&7)|((rex_b || r&8)?8:0)); }
    std::string Reg32(int r) const { int ext=(r&7)|((rex_b||r&8)?8:0); return kReg32(ext); }
    std::string Reg64(int r) const { int ext=(r&7)|((rex_b||r&8)?8:0); return kReg64(ext); }
};

// ---------------------------------------------------------------------------
// Operand size
// ---------------------------------------------------------------------------
enum class Opsize { B, W, D, Q, DQ, O };

// ---------------------------------------------------------------------------
// Operand formatting helpers
// ---------------------------------------------------------------------------
static std::string Hex(u64 v, int digits = 0) {
    char b[24];
    if (digits <= 0) std::snprintf(b, sizeof(b), "0x%llx", (unsigned long long)v);
    else std::snprintf(b, sizeof(b), "0x%0*llx", digits, (unsigned long long)v);
    return b;
}
static std::string HexByte(u8 v) { return Hex(v, 2); }

static std::string MemRef(State& s, u8 mod, u8 rm, Opsize /*os*/) {
    if (mod == 3) return "?"; // register form — handled by caller
    // SIB?
    bool has_sib = (rm == 4 && mod != 3);
    u8 sib_scale=1, sib_index=4, sib_base=rm;
    if (has_sib) {
        u8 sib = s.NextByte();
        sib_scale = 1 << ((sib>>6)&3);
        sib_index = (sib>>3)&7;
        sib_base  = sib & 7;
    }

    s64 disp = 0;
    if (mod == 1) { disp = s.NextS8(); }
    else if (mod == 2) { u32 d = s.NextU32(); disp = static_cast<s32>(d); }
    else if (mod == 0 && (rm == 5 || (has_sib && sib_base == 5))) {
        u32 d = s.NextU32(); disp = static_cast<s32>(d);
    }

    // RIP-relative
    if (mod == 0 && rm == 5 && !has_sib) {
        if (disp == 0) return "[rip]";
        char b[64]; std::snprintf(b,sizeof(b),"[rip%+lld]",(long long)disp); return b;
    }

    // Build effective address
    std::string ea;
    bool need_plus = false;

    if (!has_sib) {
        static const char* kAddr16[] = {"bx+si","bx+di","bp+si","bp+di","si","di","bp","bx"};
        const char* base = (s.addr_66 ? kAddr16[rm]
                                       : (s.rex_w ? kReg64(rm) : kReg32(rm)));
        if (mod == 0 && rm == 6) { // disp32 only
            ea = Hex((u64)(u32)disp);
            return "[" + ea + "]";
        }
        ea = base;
    } else {
        // SIB
        if (sib_base != 5 || mod != 0) {
            ea = (s.rex_w ? kReg64(sib_base) : kReg32(sib_base));
            need_plus = true;
        }
        if (sib_index != 4) {
            std::string idx = s.rex_x ? kReg64(sib_index | 8) : (s.rex_w ? kReg64(sib_index) : kReg32(sib_index));
            if (!ea.empty()) ea += "+";
            ea += idx;
            if (sib_scale != 1) ea += "*" + std::to_string(sib_scale);
            need_plus = true;
        }
    }

    if (disp != 0 || (mod == 0 && ((has_sib && sib_base == 5) || rm == 5))) {
        if (need_plus && disp >= 0) ea += "+";
        ea += std::to_string(disp);
    }

    return "[" + ea + "]";
}

static std::string RegOrMem(State& s, u8 mod, u8 rm, Opsize os) {
    if (mod == 3) {
        switch (os) {
            case Opsize::B:  return s.Reg8(rm);
            case Opsize::W:  return s.Reg16(rm);
            case Opsize::D:  return s.Reg32(rm);
            case Opsize::Q:  return s.Reg64(rm);
            case Opsize::DQ: return kXmm(rm | (s.rex_b ? 8 : 0));
            default:         return s.Reg64(rm);
        }
    }
    return MemRef(s, mod, rm, os);
}

static std::string Reg(State& s, int reg, Opsize os) {
    int r = reg | (s.rex_r ? 8 : 0);
    switch (os) {
        case Opsize::B:  return s.Reg8(r);  // low-byte: al/cl/dl/bl/ah/ch/dh/bh → reg 0-7 only
        case Opsize::W:  return s.Reg16(r);
        case Opsize::D:  return s.Reg32(r);
        case Opsize::Q:  return s.Reg64(r);
        case Opsize::DQ: return kXmm(r);
        default:         return s.Reg64(r);
    }
}

static std::string Imm8 (State& s) { return HexByte(s.NextByte()); }
static std::string Imm16(State& s) { u8 lo=s.NextByte(),hi=s.NextByte(); return Hex((hi<<8)|lo,4); }
static std::string Imm32(State& s) { u32 v=s.NextU32(); return Hex(v,8); }
static std::string Imm64(State& s) { u64 lo=s.NextU32(), hi=s.NextU32(); return Hex((hi<<32)|lo,16); }
static std::string Rel8(State& s)  { s8 r=s.NextS8(); u64 tgt=reinterpret_cast<u64>(s.pos)+r; return Hex(tgt,16); }
static std::string Rel32(State& s) { u32 d=s.NextU32(); s32 r=static_cast<s32>(d); u64 tgt=reinterpret_cast<u64>(s.pos)+r; return Hex(tgt,16); }

// ---------------------------------------------------------------------------
} // namespace <anonymous>

// ---------------------------------------------------------------------------
// Decode — main entry point
// ---------------------------------------------------------------------------
Decoded Decode(const u8* bytes, size_t size) {
    Decoded d;
    if (!bytes || size == 0) { d.text = "?? <no bytes>"; return d; }

    State s;
    s.start = bytes;
    s.pos   = bytes;
    s.end   = bytes + size;

    // --- prefix loop ---
    for (;;) {
        u8 b = *(s.pos);
        switch (b) {
            case 0xF0: s.lock=true;  s.NextByte(); continue;
            case 0xF2: s.repne=true; s.NextByte(); continue;
            case 0xF3: s.rep=true;   s.NextByte(); continue;
            case 0x66: s.opsize_66=true; s.NextByte(); continue;
            case 0x67: s.addr_66=true;   s.NextByte(); continue;
            case 0x26: case 0x2E: case 0x36: case 0x3E:
                s.NextByte(); continue; // segment overrides (ignore)
            case 0x64: s.seg_fs=true; s.NextByte(); continue;
            case 0x65: s.seg_gs=true; s.NextByte(); continue;
            default: break;
        }
        if (b >= 0x40 && b <= 0x4F) { // REX
            s.has_rex = true;
            s.rex_w = (b & 0x08) != 0;
            s.rex_r = (b & 0x04) != 0;
            s.rex_x = (b & 0x02) != 0;
            s.rex_b = (b & 0x01) != 0;
            s.NextByte();
        } else break;
    }

    if (s.pos >= s.end) { d.text = "?? <truncated>"; d.length = static_cast<u32>(s.pos - s.start); return d; }

    u8 op1 = s.NextByte();
    u8 op2 = 0, op3 = 0;
    bool is_0f = (op1 == 0x0F);
    bool is_0f38 = false, is_0f3a = false;

    if (is_0f && s.pos < s.end) {
        op2 = s.NextByte();
        if (op2 == 0x38 && s.pos < s.end) { is_0f38 = true; op3 = s.NextByte(); }
        else if (op2 == 0x3A && s.pos < s.end) { is_0f3a = true; op3 = s.NextByte(); }
    }

    // -- ModRM decode (many instructions) --
    u8 modrm = 0;
    bool has_modrm = false;
    u8 mod = 0, reg = 0, rm = 0;

    // Instructions that always have ModRM (most of them)
    bool op_expects_modrm = true;

    // Quick check: common 1-byte opcodes WITHOUT ModRM
    switch (op1) {
        case 0x90: // NOP
        case 0xCC: // INT3
        case 0xC3: // RET
        case 0xC2: // RET imm16
        case 0xCB: // RETF
        case 0x9C: // PUSHFQ
        case 0x9D: // POPFQ
        case 0xEB: // JMP rel8
        case 0xE9: // JMP rel32
        case 0xE8: // CALL rel32
        case 0x70: case 0x71: case 0x72: case 0x73: // Jcc rel8
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        case 0xCD: // INT imm8
            op_expects_modrm = false;
            break;
        default:
            // Check for mov reg, imm64 (B8-BF + REX.W)
            if ((op1 & 0xF8) == 0xB8) { op_expects_modrm = false; break; }
            // Check for push reg (50-57)
            if (op1 >= 0x50 && op1 <= 0x57) { op_expects_modrm = false; break; }
            // Check for pop reg (58-5F)
            if (op1 >= 0x58 && op1 <= 0x5F) { op_expects_modrm = false; break; }
            // PUSH imm8 (6A)
            if (op1 == 0x6A) { op_expects_modrm = false; break; }
            // PUSH imm32 (68)
            if (op1 == 0x68) { op_expects_modrm = false; break; }
            // imul (69/6B)
            if (op1 == 0x69 || op1 == 0x6B) break;
            break;
    }

    if (op_expects_modrm && s.pos < s.end) {
        modrm = s.NextByte(); has_modrm = true;
        mod = (modrm >> 6) & 3;
        reg = (modrm >> 3) & 7;
        rm  = modrm & 7;
    }

    // --- Build the mnemonic text ---
    Opsize os = Opsize::D; // default 32-bit operand
    std::string out;
    bool known = true;

    // --- Massive opcode switch ---
    // Format: mnemonic dst, src   (Intel syntax)

    // --- 1-byte ops ---
    switch (op1) {
        // NOP
        case 0x90: out = "nop"; break;

        // INT3
        case 0xCC: out = "int3"; break;

        // RET
        case 0xC3: out = "ret"; break;
        case 0xC2: out = "ret " + Imm16(s); break;
        case 0xCB: out = "retf"; break;

        // PUSHFQ / POPFQ
        case 0x9C: out = "pushfq"; break;
        case 0x9D: out = "popfq"; break;

        // PUSH reg (50-57)
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            out = "push " + s.Reg64(op1 & 7);
            break;

        // POP reg (58-5F)
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            out = "pop " + s.Reg64(op1 & 7);
            break;

        // PUSH imm (6A/68)
        case 0x6A: out = "push " + Imm8(s); break;
        case 0x68: out = "push " + Imm32(s); break;

        // MOV reg64, imm64 (B8-BF + REX.W)
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            if (s.rex_w) out = "mov " + s.Reg64(op1 & 7) + ", " + Imm64(s);
            else if (s.opsize_66) out = "mov " + s.Reg16(op1 & 7) + ", " + Imm16(s);
            else out = "mov " + s.Reg32(op1 & 7) + ", " + Imm32(s);
            break;

        // MOV r8, imm8 (B0-B7)
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            out = "mov " + s.Reg8(op1 & 7) + ", " + Imm8(s);
            break;

        // MOV AL/AX/EAX/RAX, [moffs] (A0-A3)
        case 0xA0: out = std::string("mov al,[") + Imm64(s) + "]"; break;
        case 0xA1: out = std::string("mov ") + (s.rex_w ? "rax" : s.opsize_66 ? "ax" : "eax") + ",[" + Imm64(s) + "]"; break;
        case 0xA2: out = std::string("mov [") + Imm64(s) + "],al"; break;
        case 0xA3: out = std::string("mov [") + Imm64(s) + "]," + (s.rex_w ? "rax" : s.opsize_66 ? "ax" : "eax"); break;

        // JMP rel8/32 (EB/E9)
        case 0xEB: out = "jmp " + Rel8(s); break;
        case 0xE9: out = "jmp " + Rel32(s); break;

        // CALL rel32 (E8)
        case 0xE8: out = "call " + Rel32(s); break;

        // Jcc rel8 (70-7F)
        case 0x70: out = "jo " + Rel8(s); break;
        case 0x71: out = "jno " + Rel8(s); break;
        case 0x72: out = "jb " + Rel8(s); break;
        case 0x73: out = "jnb " + Rel8(s); break;
        case 0x74: out = "jz " + Rel8(s); break;
        case 0x75: out = "jnz " + Rel8(s); break;
        case 0x76: out = "jbe " + Rel8(s); break;
        case 0x77: out = "ja " + Rel8(s); break;
        case 0x78: out = "js " + Rel8(s); break;
        case 0x79: out = "jns " + Rel8(s); break;
        case 0x7A: out = "jp " + Rel8(s); break;
        case 0x7B: out = "jnp " + Rel8(s); break;
        case 0x7C: out = "jl " + Rel8(s); break;
        case 0x7D: out = "jge " + Rel8(s); break;
        case 0x7E: out = "jle " + Rel8(s); break;
        case 0x7F: out = "jg " + Rel8(s); break;

        // INT imm8
        case 0xCD: out = "int " + Imm8(s); break;

        // ModRM-based instructions
        // MOV [rm], reg (88/89)
        case 0x88:
            os = Opsize::B;
            out = "mov " + RegOrMem(s, mod, rm, Opsize::B) + ", " + Reg(s, reg, Opsize::B);
            break;
        case 0x89:
            os = s.rex_w ? Opsize::Q : Opsize::D;
            out = "mov " + RegOrMem(s, mod, rm, os) + ", " + Reg(s, reg, os);
            break;

        // MOV reg, [rm] (8A/8B)
        case 0x8A:
            os = Opsize::B;
            out = "mov " + Reg(s, reg, Opsize::B) + ", " + RegOrMem(s, mod, rm, Opsize::B);
            break;
        case 0x8B:
            os = s.rex_w ? Opsize::Q : Opsize::D;
            out = "mov " + Reg(s, reg, os) + ", " + RegOrMem(s, mod, rm, os);
            break;

        // MOVSXD (63)
        case 0x63:
            if (s.rex_w) {
                out = "movsxd " + s.Reg64(reg) + ", " + RegOrMem(s, mod, rm, Opsize::D);
            } else if (s.opsize_66) {
                out = "movsx " + s.Reg16(reg) + ", " + RegOrMem(s, mod, rm, Opsize::B);
            } else {
                out = "movsx " + s.Reg32(reg) + ", " + RegOrMem(s, mod, rm, Opsize::W);
            }
            break;

        // LEA (8D)
        case 0x8D:
            os = s.rex_w ? Opsize::Q : Opsize::D;
            out = "lea " + Reg(s, reg, os) + ", " + RegOrMem(s, mod, rm, os);
            break;

        // MOV [rm], imm (C6/C7)
        case 0xC6:
            out = "mov " + RegOrMem(s, mod, rm, Opsize::B) + ", " + Imm8(s);
            break;
        case 0xC7:
            os = s.rex_w ? Opsize::Q : Opsize::D;
            out = "mov " + RegOrMem(s, mod, rm, os) + ", " + (s.rex_w ? Imm32(s) : (s.opsize_66 ? Imm16(s) : Imm32(s)));
            break;

        // CMP (38-3D)
        case 0x38: os=Opsize::B; out="cmp "+RegOrMem(s,mod,rm,Opsize::B)+", "+Reg(s,reg,Opsize::B); break;
        case 0x39: os=s.rex_w?Opsize::Q:Opsize::D; out="cmp "+RegOrMem(s,mod,rm,os)+", "+Reg(s,reg,os); break;
        case 0x3A: os=Opsize::B; out="cmp "+Reg(s,reg,Opsize::B)+", "+RegOrMem(s,mod,rm,Opsize::B); break;
        case 0x3B: os=s.rex_w?Opsize::Q:Opsize::D; out="cmp "+Reg(s,reg,os)+", "+RegOrMem(s,mod,rm,os); break;
        case 0x3C:
            if (s.opsize_66) out="cmp ax, "+Imm16(s); else out="cmp al, "+Imm8(s);
            break;
        case 0x3D:
            if (s.rex_w) out="cmp rax, "+Imm32(s); else if (s.opsize_66) out="cmp ax, "+Imm16(s); else out="cmp eax, "+Imm32(s);
            break;

        // Group instructions via ModRM.reg
        // 80-83: arithmetic group
        case 0x80:
        case 0x81:
        case 0x82:
        case 0x83: {
            const char* mnem = nullptr;
            switch (reg) {
                case 0: mnem="add"; break; case 1: mnem="or"; break;
                case 2: mnem="adc"; break; case 3: mnem="sbb"; break;
                case 4: mnem="and"; break; case 5: mnem="sub"; break;
                case 6: mnem="xor"; break; case 7: mnem="cmp"; break;
            }
            if (op1 == 0x80) { // byte operands
                out = std::string(mnem) + " " + RegOrMem(s, mod, rm, Opsize::B) + ", " + Imm8(s);
            } else if (op1 == 0x83) { // sign-extended imm8
                os = s.rex_w ? Opsize::Q : Opsize::D;
                if (s.opsize_66) os = Opsize::W;
                out = std::string(mnem) + " " + RegOrMem(s, mod, rm, os) + ", " + Imm8(s);
            } else { // imm16/32
                os = s.rex_w ? Opsize::Q : Opsize::D;
                if (s.opsize_66) { os = Opsize::W; out = std::string(mnem) + " " + RegOrMem(s, mod, rm, os) + ", " + Imm16(s); }
                else out = std::string(mnem) + " " + RegOrMem(s, mod, rm, os) + ", " + Imm32(s);
            }
            break;
        }

        // F6/F7 — NOT/NEG/MUL/IMUL/DIV/IDIV group
        case 0xF6:
        case 0xF7: {
            const char* mnem = nullptr;
            switch (reg) {
                case 0: out="test "+RegOrMem(s,mod,rm,Opsize::B)+", "+Imm8(s); break;
                case 1: out="test "+RegOrMem(s,mod,rm,(op1==0xF6?Opsize::B:s.rex_w?Opsize::Q:Opsize::D))+", "+((op1==0xF6)?Imm8(s):(s.rex_w?Imm32(s):Imm32(s))); break;
                case 2: mnem="not"; break; case 3: mnem="neg"; break;
                case 4: out="mul "+RegOrMem(s,mod,rm,(op1==0xF6?Opsize::B:Opsize::Q)); break;
                case 5: out="imul "+RegOrMem(s,mod,rm,(op1==0xF6?Opsize::B:Opsize::Q)); break;
                case 6: out="div "+RegOrMem(s,mod,rm,(op1==0xF6?Opsize::B:Opsize::Q)); break;
                case 7: out="idiv "+RegOrMem(s,mod,rm,(op1==0xF6?Opsize::B:Opsize::Q)); break;
            }
            if (mnem) out = std::string(mnem) + " " + RegOrMem(s, mod, rm, (op1==0xF6?Opsize::B:Opsize::Q));
            break;
        }

        // FF group
        case 0xFF: {
            switch (reg) {
                case 0: out="inc "+RegOrMem(s,mod,rm,s.rex_w?Opsize::Q:Opsize::D); break;
                case 1: out="dec "+RegOrMem(s,mod,rm,s.rex_w?Opsize::Q:Opsize::D); break;
                case 2: out="call "+RegOrMem(s,mod,rm,s.rex_w?Opsize::Q:Opsize::D); break;
                case 3: out="call far "+RegOrMem(s,mod,rm,s.rex_w?Opsize::Q:Opsize::D); break;
                case 4: out="jmp "+RegOrMem(s,mod,rm,s.rex_w?Opsize::Q:Opsize::D); break;
                case 5: out="jmp far "+RegOrMem(s,mod,rm,s.rex_w?Opsize::Q:Opsize::D); break;
                case 6: out="push "+RegOrMem(s,mod,rm,s.rex_w?Opsize::Q:Opsize::D); break;
                case 7: known=false; break;
            }
            break;
        }

        // FE group
        case 0xFE: {
            switch (reg) {
                case 0: out="inc "+RegOrMem(s,mod,rm,Opsize::B); break;
                case 1: out="dec "+RegOrMem(s,mod,rm,Opsize::B); break;
                default: known=false; break;
            }
            break;
        }

        // 84/85 TEST
        case 0x84: out="test "+RegOrMem(s,mod,rm,Opsize::B)+", "+Reg(s,reg,Opsize::B); break;
        case 0x85:
            os = s.rex_w ? Opsize::Q : Opsize::D;
            out = "test " + RegOrMem(s, mod, rm, os) + ", " + Reg(s, reg, os);
            break;

        // TEST AL/AX/EAX/RAX, imm (A8/A9)
        case 0xA8: out="test al, "+Imm8(s); break;
        case 0xA9:
            if (s.rex_w) out="test rax, "+Imm32(s); else if (s.opsize_66) out="test ax, "+Imm16(s); else out="test eax, "+Imm32(s);
            break;

        // IMUL reg, rm (0F AF)
        case 0x69:
            os=s.rex_w?Opsize::Q:Opsize::D;
            if (s.opsize_66) os=Opsize::W;
            out="imul "+Reg(s,reg,os)+", "+RegOrMem(s,mod,rm,os)+", "+Imm32(s);
            break;
        case 0x6B:
            os=s.rex_w?Opsize::Q:Opsize::D;
            if (s.opsize_66) os=Opsize::W;
            out="imul "+Reg(s,reg,os)+", "+RegOrMem(s,mod,rm,os)+", "+Imm8(s);
            break;

        // Just MOVZX / MOVSX / MOVZX
        // (These are 0F B6/B7/BE/BF — handled below in 0F switch)

        // Other ModRM opcodes as pattern for the remaining known ops
        case 0x00: os=Opsize::B; out="add "+RegOrMem(s,mod,rm,(const Opsize)Opsize::B)+", "+Reg(s,reg,(const Opsize)Opsize::B); break;
        case 0x01: os=s.rex_w?Opsize::Q:Opsize::D; out="add "+RegOrMem(s,mod,rm,os)+", "+Reg(s,reg,os); break;
        case 0x02: os=Opsize::B; out="add "+Reg(s,reg,(const Opsize)Opsize::B)+", "+RegOrMem(s,mod,rm,(const Opsize)Opsize::B); break;
        case 0x03: os=s.rex_w?Opsize::Q:Opsize::D; out="add "+Reg(s,reg,os)+", "+RegOrMem(s,mod,rm,os); break;
        case 0x08: os=Opsize::B; out="or "+RegOrMem(s,mod,rm,(const Opsize)Opsize::B)+", "+Reg(s,reg,(const Opsize)Opsize::B); break;
        case 0x09: os=s.rex_w?Opsize::Q:Opsize::D; out="or "+RegOrMem(s,mod,rm,os)+", "+Reg(s,reg,os); break;
        case 0x0A: os=Opsize::B; out="or "+Reg(s,reg,(const Opsize)Opsize::B)+", "+RegOrMem(s,mod,rm,(const Opsize)Opsize::B); break;
        case 0x0B: os=s.rex_w?Opsize::Q:Opsize::D; out="or "+Reg(s,reg,os)+", "+RegOrMem(s,mod,rm,os); break;
        case 0x10: os=Opsize::B; out="adc "+RegOrMem(s,mod,rm,(const Opsize)Opsize::B)+", "+Reg(s,reg,(const Opsize)Opsize::B); break;
        case 0x11: os=s.rex_w?Opsize::Q:Opsize::D; out="adc "+RegOrMem(s,mod,rm,os)+", "+Reg(s,reg,os); break;
        case 0x18: os=Opsize::B; out="sbb "+RegOrMem(s,mod,rm,(const Opsize)Opsize::B)+", "+Reg(s,reg,(const Opsize)Opsize::B); break;
        case 0x19: os=s.rex_w?Opsize::Q:Opsize::D; out="sbb "+RegOrMem(s,mod,rm,os)+", "+Reg(s,reg,os); break;
        case 0x20: os=Opsize::B; out="and "+RegOrMem(s,mod,rm,(const Opsize)Opsize::B)+", "+Reg(s,reg,(const Opsize)Opsize::B); break;
        case 0x21: os=s.rex_w?Opsize::Q:Opsize::D; out="and "+RegOrMem(s,mod,rm,os)+", "+Reg(s,reg,os); break;
        case 0x28: os=Opsize::B; out="sub "+RegOrMem(s,mod,rm,(const Opsize)Opsize::B)+", "+Reg(s,reg,(const Opsize)Opsize::B); break;
        case 0x29: os=s.rex_w?Opsize::Q:Opsize::D; out="sub "+RegOrMem(s,mod,rm,os)+", "+Reg(s,reg,os); break;
        case 0x30: os=Opsize::B; out="xor "+RegOrMem(s,mod,rm,(const Opsize)Opsize::B)+", "+Reg(s,reg,(const Opsize)Opsize::B); break;
        case 0x31: os=s.rex_w?Opsize::Q:Opsize::D; out="xor "+RegOrMem(s,mod,rm,os)+", "+Reg(s,reg,os); break;
        case 0x4A: out="dec "+(s.rex_w?s.Reg64(op1&7):s.Reg32(op1&7)); break;
        case 0x48: out=s.rex_w?"":"dec "+(s.rex_w?s.Reg64(op1&7):s.Reg32(op1&7));  if(out.empty())known=false; break;
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
            // REX only (no other opcode) — these are INC/DEC without REX
            if (!s.has_rex) { out = "inc " + s.Reg32(op1 & 7); } else { known = false; }
            break;

        default:
            // 0F-prefixed ops
            if (is_0f) {
                switch (op2) {
                    // 0F 1F — NOP (multi-byte)
                    case 0x1F: out = std::string("nop") + (has_modrm?"":" dword [rax]"); break;

                    // 0F B6/B7 — MOVZX
                    case 0xB6: out="movzx "+Reg(s,reg,s.rex_w?Opsize::Q:Opsize::D)+", "+RegOrMem(s,mod,rm,Opsize::B); break;
                    case 0xB7: out="movzx "+Reg(s,reg,s.rex_w?Opsize::Q:Opsize::D)+", "+RegOrMem(s,mod,rm,Opsize::W); break;

                    // 0F BE/BF — MOVSX
                    case 0xBE: out="movsx "+Reg(s,reg,s.rex_w?Opsize::Q:Opsize::D)+", "+RegOrMem(s,mod,rm,Opsize::B); break;
                    case 0xBF: out="movsx "+Reg(s,reg,s.rex_w?Opsize::Q:Opsize::D)+", "+RegOrMem(s,mod,rm,Opsize::W); break;

                    // 0F 10/11 — MOVUPS/MOVSS/MOVSD (F3/F2 prefix controlled)
                    case 0x10:
                        if (s.rep) out="movss "+Reg(s,reg,Opsize::DQ)+", "+RegOrMem(s,mod,rm,Opsize::DQ);
                        else if (s.repne) out="movsd "+Reg(s,reg,Opsize::DQ)+", "+RegOrMem(s,mod,rm,Opsize::DQ);
                        else out="movups "+Reg(s,reg,Opsize::DQ)+", "+RegOrMem(s,mod,rm,Opsize::DQ);
                        break;
                    case 0x11:
                        if (s.rep) out="movss "+RegOrMem(s,mod,rm,Opsize::DQ)+", "+Reg(s,reg,Opsize::DQ);
                        else if (s.repne) out="movsd "+RegOrMem(s,mod,rm,Opsize::DQ)+", "+Reg(s,reg,Opsize::DQ);
                        else out="movups "+RegOrMem(s,mod,rm,Opsize::DQ)+", "+Reg(s,reg,Opsize::DQ);
                        break;

                    // 0F 28/29 — MOVAPS
                    case 0x28: out="movaps "+Reg(s,reg,Opsize::DQ)+", "+RegOrMem(s,mod,rm,Opsize::DQ); break;
                    case 0x29: out="movaps "+RegOrMem(s,mod,rm,Opsize::DQ)+", "+Reg(s,reg,Opsize::DQ); break;

                    // 0F 84-8F — Jcc rel32
                    case 0x84: out="jz "+Rel32(s); break;
                    case 0x85: out="jnz "+Rel32(s); break;
                    case 0x86: out="jbe "+Rel32(s); break;
                    case 0x87: out="ja "+Rel32(s); break;
                    case 0x8C: out="jl "+Rel32(s); break;
                    case 0x8D: out="jge "+Rel32(s); break;
                    case 0x8E: out="jle "+Rel32(s); break;
                    case 0x8F: out="jg "+Rel32(s); break;

                    // 0F AF — IMUL
                    case 0xAF: os=s.rex_w?Opsize::Q:Opsize::D; out="imul "+Reg(s,reg,os)+", "+RegOrMem(s,mod,rm,os); break;

                    default: known = false; break;
                }
            } else {
                known = false;
            }
            break;
    } // switch(op1)

    // --- Build final output ---
    u32 consumed = static_cast<u32>(s.pos - s.start);
    if (known) {
        d.text   = out;
        d.known  = true;
        d.length = consumed;
    } else {
        // Unknown — emit raw hex
        char buf[128];
        int off = 0;
        off += std::snprintf(buf + off, sizeof(buf) - off, "??");
        for (const u8* p = s.start; p < s.pos && p < s.start + 15; ++p) {
            off += std::snprintf(buf + off, sizeof(buf) - off, " %02X", *p);
        }
        d.text   = buf;
        d.known  = false;
        d.length = (consumed > 0) ? consumed : static_cast<u32>(s.pos - s.start);
    }

    return d;
}

std::string DecodeOneLine(const u8* bytes, size_t size) {
    Decoded d = Decode(bytes, size);
    // Trim trailing whitespace
    std::string s = d.text;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

} // namespace InstrDecode
