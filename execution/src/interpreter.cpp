#include <pcsx5/execution/interpreter.h>
#include <bit>
#include <limits>

namespace pcsx5::execution {
namespace {
constexpr std::uint64_t cf = 1, pf = 4, af = 16, zf = 64, sf = 128, of = 2048;
constexpr std::size_t rsp = 4;

constexpr bool canonical(std::uint64_t address) noexcept {
    return address <= 0x00007fffffffffffULL || address >= 0xffff800000000000ULL;
}
constexpr std::uint64_t sign_extend(std::uint64_t value, unsigned bits) noexcept {
    const auto sign = std::uint64_t{1} << (bits - 1);
    return (value ^ sign) - sign;
}

class instruction_reader {
public:
    instruction_reader(const cpu_state& state, core::guest_memory& memory,
        step_result& result) noexcept : state_(state), memory_(memory), result_(result) {}

    bool address(std::uint64_t location, std::size_t size, access_kind access) noexcept {
        if (!canonical(location) || size - 1 > std::numeric_limits<std::uint64_t>::max() - location ||
            !canonical(location + size - 1)) {
            result_.reason = stop_reason::invalid_address;
            result_.access = access;
            result_.fault_address = location;
            return false;
        }
        return true;
    }
    bool bytes(unsigned count, std::uint64_t& value) noexcept {
        value = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (result_.length == result_.instruction.size()) {
                result_.reason = stop_reason::unsupported;
                return false;
            }
            if (state_.rip > std::numeric_limits<std::uint64_t>::max() - result_.length) {
                result_.reason = stop_reason::invalid_address;
                result_.access = access_kind::fetch;
                result_.fault_address = state_.rip;
                return false;
            }
            const auto location = state_.rip + result_.length;
            if (!address(location, 1, access_kind::fetch)) return false;
            std::byte byte{};
            const auto read = memory_.read(location, std::span{&byte, 1});
            if (!read) return failure(location, access_kind::fetch, read.error());
            result_.instruction[result_.length++] = byte;
            value |= std::uint64_t{std::to_integer<unsigned>(byte)} << (i * 8);
        }
        return true;
    }
    bool load(std::uint64_t location, unsigned size, std::uint64_t& value) noexcept {
        if (!address(location, size, access_kind::read)) return false;
        std::array<std::byte, 8> data{};
        const auto read = memory_.read(location, std::span{data}.first(size));
        if (!read) return failure(location, access_kind::read, read.error());
        value = 0;
        for (unsigned i = 0; i < size; ++i)
            value |= std::uint64_t{std::to_integer<unsigned>(data[i])} << (8 * i);
        return true;
    }
    bool store(std::uint64_t location, unsigned size, std::uint64_t value) noexcept {
        if (!address(location, size, access_kind::write)) return false;
        std::array<std::byte, 8> data{};
        for (unsigned i = 0; i < size; ++i)
            data[i] = static_cast<std::byte>((value >> (8 * i)) & 255);
        const auto write = memory_.write(location, std::span{data}.first(size));
        if (!write) {
            result_.memory_uncertain = true;
            return failure(location, access_kind::write, write.error());
        }
        result_.wrote_memory = true;
        result_.write_address = location;
        result_.write_size = static_cast<std::uint8_t>(size);
        result_.write_bytes = data;
        return true;
    }
private:
    bool failure(std::uint64_t location, access_kind access, core::guest_memory_error error) noexcept {
        result_.reason = stop_reason::memory_failure;
        result_.access = access;
        result_.memory_error = error;
        result_.fault_address = location;
        return false;
    }
    const cpu_state& state_;
    core::guest_memory& memory_;
    step_result& result_;
};

// Effective address construction is modulo 64 bits; canonicality is checked
// only on the resulting access, not on the individual base/index registers.
bool effective_address(instruction_reader& reader, const cpu_state& state,
    const step_result& result, std::uint64_t modrm, std::uint64_t rex,
    std::uint64_t& address) noexcept {
    const auto mod = modrm >> 6;
    const auto rm = modrm & 7;
    bool relative = false;
    bool displacement32 = mod == 2;
    address = 0;
    if (rm == 4) {
        std::uint64_t sib{};
        if (!reader.bytes(1, sib)) return false;
        const auto base = sib & 7;
        const auto index = (sib >> 3) & 7;
        if (index != 4 || (rex & 2) != 0)
            address = state.gpr[index + ((rex & 2) != 0 ? 8 : 0)] << (sib >> 6);
        if (mod == 0 && base == 5) displacement32 = true;
        else address += state.gpr[base + ((rex & 1) != 0 ? 8 : 0)];
    } else if (mod == 0 && rm == 5) {
        relative = true;
        displacement32 = true;
    } else address = state.gpr[rm + ((rex & 1) != 0 ? 8 : 0)];
    if (mod == 1 || displacement32) {
        std::uint64_t displacement{};
        const unsigned size = mod == 1 ? 1 : 4;
        if (!reader.bytes(size, displacement)) return false;
        address += sign_extend(displacement, size * 8);
    }
    // MOV has no trailing immediate, so this is the complete next RIP.
    if (relative) address += state.rip + result.length;
    return true;
}

enum class operation { add, bit_or, bit_and, sub, bit_xor, cmp, test };
std::uint64_t arithmetic(cpu_state& state, operation op, std::uint64_t left,
    std::uint64_t right, bool wide) noexcept {
    const auto mask = wide ? ~std::uint64_t{0} : std::uint64_t{0xffffffff};
    const auto sign = std::uint64_t{1} << (wide ? 63 : 31);
    left &= mask;
    right &= mask;
    std::uint64_t value{};
    std::uint64_t flags{};
    const bool addition = op == operation::add;
    const bool subtraction = op == operation::sub || op == operation::cmp;
    if (addition || subtraction) {
        value = (addition ? left + right : left - right) & mask;
        if (addition ? value < left : left < right) flags |= cf;
        if (((left ^ right ^ value) & 16) != 0) flags |= af;
        if ((addition ? (~(left ^ right) & (left ^ value) & sign) :
                ((left ^ right) & (left ^ value) & sign)) != 0) flags |= of;
        state.known_flags |= arithmetic_flags;
    } else {
        if (op == operation::bit_or) value = left | right;
        else if (op == operation::bit_xor) value = left ^ right;
        else value = left & right;
        state.known_flags = (state.known_flags | arithmetic_flags) & ~af;
    }
    if (value == 0) flags |= zf;
    if ((value & sign) != 0) flags |= sf;
    if ((std::popcount(static_cast<unsigned>(value & 255)) & 1) == 0) flags |= pf;
    state.rflags = (state.rflags & ~arithmetic_flags) | flags;
    return value;
}

bool condition(cpu_state& state, unsigned code, bool& taken) noexcept {
    constexpr std::array<std::uint64_t, 8> required{of, cf, zf, cf | zf, sf, pf, sf | of, zf | sf | of};
    if ((state.known_flags & required[code / 2]) != required[code / 2]) return false;
    const auto flags = state.rflags;
    switch (code / 2) {
    case 0: taken = (flags & of) != 0; break;
    case 1: taken = (flags & cf) != 0; break;
    case 2: taken = (flags & zf) != 0; break;
    case 3: taken = (flags & (cf | zf)) != 0; break;
    case 4: taken = (flags & sf) != 0; break;
    case 5: taken = (flags & pf) != 0; break;
    case 6: taken = ((flags & sf) != 0) != ((flags & of) != 0); break;
    default: taken = (flags & zf) != 0 || (((flags & sf) != 0) != ((flags & of) != 0)); break;
    }
    if ((code & 1) != 0) taken = !taken;
    return true;
}
} // namespace

step_result step(cpu_state& state, core::guest_memory& memory) noexcept {
    step_result result{};
    instruction_reader reader{state, memory, result};
    cpu_state next = state;
    std::uint64_t opcode{}, rex{};
    if (!reader.bytes(1, opcode)) return result;
    if (opcode >= 0x40 && opcode <= 0x4f) {
        rex = opcode;
        if (!reader.bytes(1, opcode)) return result;
    }
    const bool wide = (rex & 8) != 0;
    bool store_pending = false;
    std::uint64_t store_address{}, store_value{};
    unsigned store_size{};
    bool branch = false;
    std::uint64_t branch_target{};
    const auto unsupported = [&]() { result.reason = stop_reason::unsupported; return result; };
    if (opcode >= 0xb8 && opcode <= 0xbf) {
        std::uint64_t immediate{};
        if (!reader.bytes(wide ? 8 : 4, immediate)) return result;
        next.gpr[opcode - 0xb8 + ((rex & 1) != 0 ? 8 : 0)] = immediate;
    } else if (opcode == 0x89 || opcode == 0x8b) {
        std::uint64_t modrm{};
        if (!reader.bytes(1, modrm)) return result;
        const auto reg = ((modrm >> 3) & 7) + ((rex & 4) != 0 ? 8 : 0);
        const auto rm = (modrm & 7) + ((rex & 1) != 0 ? 8 : 0);
        const auto mask = wide ? ~std::uint64_t{0} : std::uint64_t{0xffffffff};
        if ((modrm >> 6) == 3) {
            if (opcode == 0x89) next.gpr[rm] = state.gpr[reg] & mask;
            else next.gpr[reg] = state.gpr[rm] & mask;
        } else {
            std::uint64_t location{};
            if (!effective_address(reader, state, result, modrm, rex, location)) return result;
            if (opcode == 0x89) {
                store_pending = true; store_address = location;
                store_value = state.gpr[reg]; store_size = wide ? 8 : 4;
            } else if (!reader.load(location, wide ? 8 : 4, next.gpr[reg])) return result;
        }
    } else if (opcode == 0x90 && rex == 0) {
        // Architectural NOP. REX forms can encode XCHG and are not modeled.
    } else if (opcode >= 0x50 && opcode <= 0x5f) {
        const auto reg = (opcode & 7) + ((rex & 1) != 0 ? 8 : 0);
        if (opcode < 0x58) {
            next.gpr[rsp] -= 8;
            store_pending = true; store_address = next.gpr[rsp];
            store_value = state.gpr[reg]; store_size = 8;
        } else {
            std::uint64_t value{};
            if (!reader.load(state.gpr[rsp], 8, value)) return result;
            next.gpr[rsp] += 8;
            next.gpr[reg] = value; // POP RSP replaces the incremented stack pointer.
        }
    } else if (opcode == 0xcc) {
        result.reason = stop_reason::breakpoint; return result;
    } else if (opcode == 0xc3) {
        if (!reader.load(state.gpr[rsp], 8, branch_target)) return result;
        next.gpr[rsp] += 8;
        branch = true;
    } else if (opcode == 0xe8 || opcode == 0xe9 || opcode == 0xeb ||
        (opcode >= 0x70 && opcode <= 0x7f) || opcode == 0x0f) {
        bool conditional = opcode >= 0x70 && opcode <= 0x7f;
        unsigned condition_code = static_cast<unsigned>(opcode & 15);
        if (opcode == 0x0f) {
            std::uint64_t second{};
            if (!reader.bytes(1, second)) return result;
            if (second == 5) { result.reason = stop_reason::syscall; return result; }
            if (second < 0x80 || second > 0x8f) return unsupported();
            conditional = true;
            condition_code = static_cast<unsigned>(second & 15);
        }
        const unsigned size = opcode == 0xeb || (opcode >= 0x70 && opcode <= 0x7f) ? 1 : 4;
        std::uint64_t displacement{};
        if (!reader.bytes(size, displacement)) return result;
        bool taken = true;
        if (conditional && !condition(state, condition_code, taken)) {
            result.reason = stop_reason::unknown_flags; return result;
        }
        if (taken) {
            branch = true;
            branch_target = state.rip + result.length + sign_extend(displacement, size * 8);
        }
        if (opcode == 0xe8) {
            next.gpr[rsp] -= 8;
            store_pending = true; store_address = next.gpr[rsp];
            store_value = state.rip + result.length; store_size = 8;
        }
    } else {
        operation op{};
        bool reverse = false;
        const bool immediate = opcode == 0x81 || opcode == 0x83;
        switch (opcode) {
        case 0x01: case 0x03: op = operation::add; break;
        case 0x09: case 0x0b: op = operation::bit_or; break;
        case 0x21: case 0x23: op = operation::bit_and; break;
        case 0x29: case 0x2b: op = operation::sub; break;
        case 0x31: case 0x33: op = operation::bit_xor; break;
        case 0x39: case 0x3b: op = operation::cmp; break;
        case 0x85: op = operation::test; break;
        case 0x81: case 0x83: break;
        default: return unsupported();
        }
        std::uint64_t modrm{};
        if (!reader.bytes(1, modrm)) return result;
        if ((modrm >> 6) != 3) return unsupported();
        const auto reg = ((modrm >> 3) & 7) + ((rex & 4) != 0 ? 8 : 0);
        const auto rm = (modrm & 7) + ((rex & 1) != 0 ? 8 : 0);
        std::uint64_t right{};
        if (immediate) {
            switch ((modrm >> 3) & 7) {
            case 0: op = operation::add; break;
            case 1: op = operation::bit_or; break;
            case 4: op = operation::bit_and; break;
            case 5: op = operation::sub; break;
            case 6: op = operation::bit_xor; break;
            case 7: op = operation::cmp; break;
            default: return unsupported();
            }
            const unsigned size = opcode == 0x83 ? 1 : 4;
            if (!reader.bytes(size, right)) return result;
            right = sign_extend(right, size * 8);
        } else {
            reverse = (opcode & 2) != 0;
            right = state.gpr[reverse ? rm : reg];
        }
        const auto destination = reverse ? reg : rm;
        const auto value = arithmetic(next, op, state.gpr[destination], right, wide);
        if (op != operation::cmp && op != operation::test) next.gpr[destination] = value;
    }
    next.rip = branch ? branch_target : state.rip + result.length;
    // Validate control flow before the sole potentially irreversible effect.
    if (!reader.address(next.rip, 1, access_kind::fetch)) return result;
    if (store_pending && !reader.store(store_address, store_size, store_value)) return result;
    state = next;
    return result;
}
} // namespace pcsx5::execution
