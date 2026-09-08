#include <pcsx5/execution/arm64_jit.h>
#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>

namespace pcsx5::execution {
namespace {
using op = scalar_operation;
constexpr std::uint64_t af = 0x10;
static_assert(std::is_standard_layout_v<cpu_state>);
static_assert(offsetof(cpu_state,gpr)==0 && offsetof(cpu_state,rip)==128);
static_assert(offsetof(cpu_state,rflags)==136 && offsetof(cpu_state,known_flags)==144);

// All words below are A64 encodings. Registers X9..X15 are caller-saved;
// X0 remains the sole state base, and no stack, literal pool or host pointer is used.
class emitter {
public:
    std::vector<std::byte> bytes;
    void word(std::uint32_t value) {
        for (unsigned i=0;i<4;++i) bytes.push_back(static_cast<std::byte>(value>>(8*i)));
    }
    void constant(unsigned reg,std::uint64_t value) {
        word(0xd2800000u | (static_cast<std::uint32_t>(value&65535)<<5) | reg);
        for (unsigned i=1;i<4;++i) {
            const auto part=static_cast<std::uint32_t>((value>>(16*i))&65535);
            if (part) word(0xf2800000u | (i<<21) | (part<<5) | reg);
        }
    }
    void load(unsigned reg,unsigned offset) { word(0xf9400000u | ((offset/8)<<10) | reg); }
    void store(unsigned reg,unsigned offset) { word(0xf9000000u | ((offset/8)<<10) | reg); }
    void binary(std::uint32_t base,unsigned dest,unsigned left,unsigned right,unsigned shift=0,bool right_shift=false) {
        word(base | (right<<16) | (shift<<10) | (left<<5) | dest | (right_shift?0x00400000u:0));
    }
    void bit(unsigned dest,unsigned src,unsigned index) {
        word(0xd3400000u | (index<<16) | (index<<10) | (src<<5) | dest); // UBFX width 1
    }
    void flags(bool subtract,bool logical) {
        word(0xd53b420cu); // MRS X12,NZCV, immediately after flag-setting ALU
        load(14,136);
        constant(15,~arithmetic_flags);
        binary(0x8a000000u,14,14,15); // preserve unmodeled RFLAGS bits
        bit(13,12,31); binary(0xaa000000u,14,14,13,7); // N -> SF
        bit(13,12,30); binary(0xaa000000u,14,14,13,6); // Z -> ZF
        if (!logical) {
            bit(13,12,28); binary(0xaa000000u,14,14,13,11); // V -> OF
            bit(13,12,29);
            if (subtract) { constant(15,1); binary(0xca000000u,13,13,15); }
            binary(0xaa000000u,14,14,13); // C or !C -> CF
            binary(0xca000000u,13,9,10);
            binary(0xca000000u,13,13,11);
            constant(15,af); binary(0x8a000000u,13,13,15);
            binary(0xaa000000u,14,14,13); // half carry
        }
        // Fold precisely the low eight result bits to even parity.
        binary(0xca000000u,13,11,11,4,true);
        binary(0xca000000u,13,13,13,2,true);
        binary(0xca000000u,13,13,13,1,true);
        constant(15,1); binary(0x8a000000u,13,13,15);
        binary(0xca000000u,13,13,15);
        binary(0xaa000000u,14,14,13,2);
        store(14,136);
        load(14,144); constant(15,arithmetic_flags);
        binary(0xaa000000u,14,14,15);
        if (logical) { constant(15,~af); binary(0x8a000000u,14,14,15); }
        store(14,144);
    }
    void instruction(const scalar_instruction& in) {
        if (in.operation!=op::nop) {
            load(9,8u*in.destination);
            if (in.immediate) constant(10,in.value);
            else load(10,8u*in.source);
            const std::uint32_t wide=in.width==64?0x80000000u:0;
            const bool subtract=in.operation==op::subtract || in.operation==op::compare;
            const bool logical=in.operation==op::bit_xor || in.operation==op::bit_and || in.operation==op::bit_or || in.operation==op::test;
            switch (in.operation) {
            case op::move: binary(0x2a000000u|wide,11,31,10); break;
            case op::add: binary(0x2b000000u|wide,11,9,10); break;
            case op::subtract: case op::compare: binary(0x6b000000u|wide,11,9,10); break;
            case op::bit_and: case op::test: binary(0x6a000000u|wide,11,9,10); break;
            case op::bit_xor: binary(0x4a000000u|wide,11,9,10); binary(0x6a000000u|wide,31,11,11); break;
            case op::bit_or: binary(0x2a000000u|wide,11,9,10); binary(0x6a000000u|wide,31,11,11); break;
            case op::nop: break;
            }
            if (in.operation!=op::move) flags(subtract,logical);
            if (in.operation!=op::compare && in.operation!=op::test) store(11,8u*in.destination);
        }
        constant(9,in.rip+in.length); store(9,128);
    }
};
bool valid(const scalar_instruction& in) noexcept {
    if (in.width!=32 && in.width!=64) return false;
    if (in.destination>=16 || in.source>=16 || in.length==0 || in.length>15) return false;
    if (in.rip>std::numeric_limits<std::uint64_t>::max()-in.length) return false;
    const auto canonical=[](std::uint64_t address) {
        return address<=0x00007fffffffffffULL || address>=0xffff800000000000ULL;
    };
    if (!canonical(in.rip) || !canonical(in.rip+in.length)) return false;
    switch (in.operation) {
    case op::nop: return !in.immediate;
    case op::move: case op::add: case op::subtract: case op::compare:
    case op::bit_xor: case op::bit_and: case op::bit_or: case op::test: return true;
    }
    return false;
}
}

std::expected<scalar_instruction,jit_error> lower_scalar(std::span<const std::byte> bytes,std::uint64_t guest_rip) noexcept {
    scalar_instruction result; result.rip=guest_rip;
    std::size_t cursor{};
    const auto take=[&](unsigned count,std::uint64_t& value) {
        if (bytes.size()-cursor<count) return false;
        value=0;
        for (unsigned i=0;i<count;++i) value|=std::uint64_t(std::to_integer<unsigned>(bytes[cursor++]))<<(8*i);
        return true;
    };
    std::uint64_t opcode{},rex{};
    if (!take(1,opcode)) return std::unexpected(jit_error::truncated);
    if (opcode>=0x40 && opcode<=0x4f) {
        rex=opcode;
        if (!take(1,opcode)) return std::unexpected(jit_error::truncated);
    }
    result.width=(rex&8)?64:32;
    if (opcode==0x90 && rex==0) result.operation=op::nop;
    else if (opcode>=0xb8 && opcode<=0xbf) {
        result.operation=op::move; result.immediate=true;
        result.destination=static_cast<std::uint8_t>(opcode-0xb8+((rex&1)?8:0));
        if (!take(result.width/8,result.value)) return std::unexpected(jit_error::truncated);
    } else {
        const bool immediate=opcode==0x81 || opcode==0x83;
        switch (opcode) {
        case 0x89: case 0x8b: result.operation=op::move; break;
        case 1: case 3: result.operation=op::add; break;
        case 9: case 11: result.operation=op::bit_or; break;
        case 0x21: case 0x23: result.operation=op::bit_and; break;
        case 0x29: case 0x2b: result.operation=op::subtract; break;
        case 0x31: case 0x33: result.operation=op::bit_xor; break;
        case 0x39: case 0x3b: result.operation=op::compare; break;
        case 0x85: result.operation=op::test; break;
        case 0x81: case 0x83: break;
        default: return std::unexpected(jit_error::unsupported);
        }
        std::uint64_t modrm{};
        if (!take(1,modrm)) return std::unexpected(jit_error::truncated);
        if ((modrm>>6)!=3) return std::unexpected(jit_error::unsupported);
        const auto reg=static_cast<std::uint8_t>(((modrm>>3)&7)+((rex&4)?8:0));
        const auto rm=static_cast<std::uint8_t>((modrm&7)+((rex&1)?8:0));
        result.destination=(!immediate && (opcode&2))?reg:rm;
        result.source=(opcode&2)?rm:reg;
        if (immediate) {
            result.immediate=true;
            switch ((modrm>>3)&7) {
            case 0: result.operation=op::add; break;
            case 1: result.operation=op::bit_or; break;
            case 4: result.operation=op::bit_and; break;
            case 5: result.operation=op::subtract; break;
            case 6: result.operation=op::bit_xor; break;
            case 7: result.operation=op::compare; break;
            default: return std::unexpected(jit_error::unsupported);
            }
            const unsigned count=opcode==0x83?1:4;
            if (!take(count,result.value)) return std::unexpected(jit_error::truncated);
            const auto sign=std::uint64_t{1}<<(count*8-1);
            result.value=(result.value^sign)-sign;
        }
    }
    result.length=static_cast<std::uint8_t>(cursor);
    if (!valid(result)) return std::unexpected(jit_error::invalid);
    return result;
}

std::expected<std::vector<std::byte>,jit_error> emit_arm64(std::span<const scalar_instruction> instructions) noexcept {
    if (instructions.empty() || instructions.size()>arm64_max_instructions) return std::unexpected(jit_error::invalid);
    std::uint64_t next=instructions.front().rip;
    for (const auto& in:instructions) {
        if (!valid(in) || in.rip!=next) return std::unexpected(jit_error::invalid);
        next=in.rip+in.length;
    }
    try {
        emitter out;
        out.bytes.reserve(arm64_max_code_bytes);
        for (const auto& in:instructions) out.instruction(in);
        out.word(0xd65f03c0u); // RET X30
        if (out.bytes.size()>arm64_max_code_bytes) return std::unexpected(jit_error::invalid);
        return std::move(out.bytes);
    } catch (const std::bad_alloc&) { return std::unexpected(jit_error::out_of_memory); }
}
} // namespace pcsx5::execution
