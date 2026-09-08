#include <pcsx5/execution/interpreter.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <vector>

using namespace pcsx5::core;
using namespace pcsx5::execution;
namespace {
unsigned checks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
struct backing final : memory_backing {
    std::array<std::byte, 4096> bytes{};
    bool fail_write{};
    std::uint64_t size() const noexcept override { return bytes.size(); }
    guest_memory_result<void> read(std::uint64_t offset, std::span<std::byte> out) const noexcept override {
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin()); return {};
    }
    guest_memory_result<void> write(std::uint64_t offset, std::span<const std::byte> in) noexcept override {
        if (fail_write) { bytes[static_cast<std::size_t>(offset)] = in.front(); return std::unexpected(guest_memory_error::host_failure); }
        std::copy(in.begin(), in.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset)); return {};
    }
    guest_memory_result<void> release() noexcept override { return {}; }
};
struct fixture {
    guest_memory memory;
    backing* store{};
    cpu_state cpu{};
    fixture() {
        auto owned = std::make_unique<backing>(); store = owned.get();
        std::unique_ptr<memory_backing> base = std::move(owned);
        CHECK(memory.map(0x1000, 4096, guest_memory_access::read_write, base)); cpu.rip = 0x1000;
    }
    void code(const std::vector<unsigned>& bytes) {
        cpu.rip = 0x1000;
        for (std::size_t i = 0; i < bytes.size(); ++i) store->bytes[i] = static_cast<std::byte>(bytes[i]);
    }
    void put(std::uint64_t address, std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) store->bytes[static_cast<std::size_t>(address - 0x1000 + i)] = static_cast<std::byte>(value >> (i * 8));
    }
    std::uint64_t get(std::uint64_t address) {
        std::uint64_t result{};
        for (unsigned i = 0; i < 8; ++i) result |= std::uint64_t(std::to_integer<unsigned>(store->bytes[static_cast<std::size_t>(address - 0x1000 + i)])) << (i * 8);
        return result;
    }
};
void append(std::vector<unsigned>& bytes, std::uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<unsigned>((value >> (8 * i)) & 255));
}
// Independent bit-serial full-adder oracle: no signed overflow or native flags.
struct expected_alu { std::uint64_t value{}, flags{}, known{arithmetic_flags}; };
expected_alu oracle(unsigned operation, std::uint64_t a, std::uint64_t b, unsigned width) {
    expected_alu out;
    if (operation == 0 || operation == 1 || operation == 2) {
        const bool subtract = operation != 0;
        bool carry = subtract, into_sign{}, half{};
        for (unsigned bit = 0; bit < width; ++bit) {
            if (bit == width - 1) into_sign = carry;
            const bool x = ((a >> bit) & 1) != 0;
            const bool y = (((b >> bit) & 1) != 0) != subtract;
            if ((x != y) != carry) out.value |= std::uint64_t{1} << bit;
            carry = (x && y) || (x && carry) || (y && carry);
            if (bit == 3) half = carry;
        }
        if (carry != subtract) out.flags |= 1;
        if (half != subtract) out.flags |= 0x10;
        if (carry != into_sign) out.flags |= 0x800;
    } else {
        out.value = operation == 3 ? a ^ b : operation == 5 ? a | b : a & b;
        if (width == 32) out.value &= 0xffffffff;
        out.known &= ~std::uint64_t{0x10};
    }
    unsigned ones{};
    for (unsigned bit = 0; bit < 8; ++bit) ones += static_cast<unsigned>((out.value >> bit) & 1);
    if (ones % 2 == 0) out.flags |= 4;
    if (out.value == 0) out.flags |= 0x40;
    if ((out.value >> (width - 1)) != 0) out.flags |= 0x80;
    return out;
}
void arithmetic() {
    constexpr std::array<unsigned, 7> forward{0x01,0x29,0x39,0x31,0x21,0x09,0x85};
    constexpr std::array<unsigned, 7> reverse{0x03,0x2b,0x3b,0x33,0x23,0x0b,0x85};
    constexpr std::array<unsigned, 6> groups{0,5,7,6,4,1};
    constexpr std::array<std::uint64_t, 12> values{0,1,15,16,127,128,0x7fffffff,0x80000000,0xffffffff,0x7fffffffffffffffULL,0x8000000000000000ULL,0xffffffffffffffffULL};
    fixture f;
    for (unsigned width : {32u,64u}) for (unsigned op = 0; op < 7; ++op)
    for (auto a : values) for (auto b : values) for (unsigned direction = 0; direction < 2; ++direction) {
        std::vector<unsigned> code; if (width == 64) code.push_back(0x48);
        code.push_back(direction ? reverse[op] : forward[op]); code.push_back(direction && op != 6 ? 0xc1 : 0xc8);
        f.code(code); f.cpu.gpr[0] = a; f.cpu.gpr[1] = b; f.cpu.rflags = 0x202 | arithmetic_flags;
        const auto expected = oracle(op,a,b,width); const auto result = step(f.cpu,f.memory);
        CHECK(result.reason == stop_reason::completed); CHECK(result.length == code.size());
        CHECK(f.cpu.gpr[0] == (op == 2 || op == 6 ? a : expected.value)); CHECK(f.cpu.gpr[1] == b);
        CHECK((f.cpu.rflags & expected.known) == expected.flags); CHECK((f.cpu.known_flags & arithmetic_flags) == expected.known);
        CHECK((f.cpu.rflags & ~arithmetic_flags) == 0x202); CHECK(f.cpu.rip == 0x1000 + code.size());
    }
    for (unsigned width : {32u,64u}) for (unsigned op = 0; op < 6; ++op)
    for (auto a : values) for (unsigned short_imm : {0u,1u}) for (auto raw : {0u,1u,0x7fu,0x80u,0xffu,0x80000000u,0xffffffffu}) {
        std::vector<unsigned> code; if (width == 64) code.push_back(0x48);
        code.push_back(short_imm ? 0x83 : 0x81); code.push_back(0xc0 + groups[op] * 8);
        append(code,raw,short_imm ? 1 : 4);
        std::uint64_t b = short_imm ? raw & 255 : raw;
        if (short_imm && (b & 0x80)) b |= 0xffffffffffffff00ULL;
        if (!short_imm && width == 64 && (b & 0x80000000)) b |= 0xffffffff00000000ULL;
        f.code(code); f.cpu.gpr[0] = a;
        const auto expected = oracle(op,a,b,width); CHECK(step(f.cpu,f.memory).reason == stop_reason::completed);
        CHECK(f.cpu.gpr[0] == (op == 2 ? a : expected.value)); CHECK((f.cpu.rflags & expected.known) == expected.flags);
    }
}
void moves() {
    fixture f;
    for (unsigned width : {32u,64u}) for (unsigned reg = 0; reg < 16; ++reg) {
        std::vector<unsigned> code;
        if (width == 64 || reg >= 8) code.push_back(0x40 + (width == 64 ? 8 : 0) + (reg >= 8 ? 1 : 0));
        code.push_back(0xb8 + reg % 8); append(code,0xfedcba9876543210ULL,width / 8);
        f.code(code); f.cpu.gpr.fill(~std::uint64_t{}); CHECK(step(f.cpu,f.memory).reason == stop_reason::completed);
        CHECK(f.cpu.gpr[reg] == (width == 64 ? 0xfedcba9876543210ULL : 0x76543210ULL));
        for (unsigned other = 0; other < 16; ++other) if (other != reg) CHECK(f.cpu.gpr[other] == ~std::uint64_t{});
        for (unsigned source = 0; source < 16; ++source) for (unsigned opcode : {0x89u,0x8bu}) {
            const unsigned field = opcode == 0x89 ? source : reg, rm = opcode == 0x89 ? reg : source;
            code = {0x40u + (width == 64 ? 8u : 0u) + (field >= 8 ? 4u : 0u) + (rm >= 8 ? 1u : 0u),opcode,0xc0 + (field % 8) * 8 + rm % 8};
            f.code(code); f.cpu.gpr.fill(~std::uint64_t{}); f.cpu.gpr[source] = 0xfedcba9876543210ULL;
            CHECK(step(f.cpu,f.memory).reason == stop_reason::completed); CHECK(f.cpu.gpr[reg] == (width == 64 ? 0xfedcba9876543210ULL : 0x76543210ULL));
        }
    }
    struct address_case { std::vector<unsigned> suffix; unsigned reg; std::uint64_t value; };
    const std::array<address_case, 6> cases{{{{0x03},3,0x1800},{{0x43,0xf8},3,0x1808},{{0x83,0x00,0x08,0,0},3,0x1000},{{0x04,0x8b},3,0x17e0},{{0x05,0xf9,0x07,0,0},3,0},{{0x04,0x25,0,0x18,0,0},3,0}}};
    for (const auto& item : cases) for (unsigned width : {32u,64u}) {
        std::vector<unsigned> code{0x48,0x8b}; code.insert(code.end(),item.suffix.begin(),item.suffix.end());
        // Keep REX for both widths so RIP-relative instruction length is fixed.
        if (width == 32) code[0] = 0x40;
        f.code(code); f.cpu.gpr[item.reg] = item.value; f.cpu.gpr[1] = 8; f.put(0x1800,0x123456789abcdef0ULL);
        CHECK(step(f.cpu,f.memory).reason == stop_reason::completed); CHECK(f.cpu.gpr[0] == (width == 64 ? 0x123456789abcdef0ULL : 0x9abcdef0ULL));
        code[1] = 0x89; f.code(code); f.cpu.gpr[0] = 0x8877665544332211ULL;
        const auto result = step(f.cpu,f.memory); CHECK(result.reason == stop_reason::completed); CHECK(result.wrote_memory);
        CHECK(result.write_address == 0x1800); CHECK(result.write_size == width / 8);
        CHECK(f.get(0x1800) == (width == 64 ? 0x8877665544332211ULL : 0x1234567844332211ULL));
    }
    // REX.R/B/X all participate: r10 = [r13 + r12*4 - 8].
    f.code({0x4f,0x8b,0x54,0xa5,0xf8}); f.cpu.gpr[13] = 0x17e8; f.cpu.gpr[12] = 8;
    f.put(0x1800,0xfedcba9876543210ULL); CHECK(step(f.cpu,f.memory).reason == stop_reason::completed);
    CHECK(f.cpu.gpr[10] == 0xfedcba9876543210ULL);
    // SIB no-base still permits an extended index and a negative displacement.
    f.code({0x4a,0x8b,0x04,0x25,0xf0,0xff,0xff,0xff}); f.cpu.gpr[12] = 0x1810;
    CHECK(step(f.cpu,f.memory).reason == stop_reason::completed); CHECK(f.cpu.gpr[0] == 0xfedcba9876543210ULL);
}
bool condition(unsigned cc, unsigned mask) {
    const bool c = (mask & 1) != 0, p = (mask & 2) != 0, z = (mask & 4) != 0, s = (mask & 8) != 0, o = (mask & 16) != 0;
    const std::array<bool,16> values{o,!o,c,!c,z,!z,c||z,!c&&!z,s,!s,p,!p,s!=o,s==o,z||(s!=o),!z&&(s==o)};
    return values[cc];
}
void branches() {
    fixture f;
    for (unsigned cc = 0; cc < 16; ++cc) for (unsigned flags = 0; flags < 32; ++flags) for (bool near : {false,true}) {
        std::vector<unsigned> code = near ? std::vector<unsigned>{0x0f,0x80+cc,8,0,0,0} : std::vector<unsigned>{0x70+cc,8};
        f.code(code); f.cpu.rflags = 2 | (flags & 1) | ((flags & 2) << 1) | ((flags & 4) << 4) | ((flags & 8) << 4) | ((flags & 16) << 7);
        f.cpu.known_flags = arithmetic_flags;
        CHECK(step(f.cpu,f.memory).reason == stop_reason::completed); CHECK(f.cpu.rip == 0x1000 + code.size() + (condition(cc,flags) ? 8 : 0));
        f.code(code); f.cpu.known_flags = 0; const auto saved = f.cpu;
        CHECK(step(f.cpu,f.memory).reason == stop_reason::unknown_flags); CHECK(f.cpu == saved);
    }
    constexpr std::array<std::uint64_t,16> required{0x800,0x800,1,1,0x40,0x40,0x41,0x41,0x80,0x80,4,4,0x880,0x880,0x8c0,0x8c0};
    for (unsigned cc = 0; cc < 16; ++cc) {
        f.code({0x70 + cc,0}); f.cpu.known_flags = required[cc];
        CHECK(step(f.cpu,f.memory).reason == stop_reason::completed);
        for (unsigned bit = 0; bit < 12; ++bit) if ((required[cc] & (std::uint64_t{1} << bit)) != 0) {
            f.code({0x70 + cc,0}); f.cpu.known_flags = arithmetic_flags & ~(std::uint64_t{1} << bit);
            const auto saved = f.cpu; CHECK(step(f.cpu,f.memory).reason == stop_reason::unknown_flags); CHECK(f.cpu == saved);
        }
    }
    for (auto code : {std::vector<unsigned>{0xeb,0xfe},std::vector<unsigned>{0xe9,0xfb,0xff,0xff,0xff}}) {
        f.code(code); CHECK(step(f.cpu,f.memory).reason == stop_reason::completed); CHECK(f.cpu.rip == 0x1000);
    }
    for (unsigned reg = 0; reg < 16; ++reg) {
        std::vector<unsigned> code; if (reg >= 8) code.push_back(0x41); code.push_back(0x50 + reg % 8);
        f.code(code); f.cpu.gpr[reg] = 0x1234; f.cpu.gpr[4] = 0x1f00;
        CHECK(step(f.cpu,f.memory).reason == stop_reason::completed); CHECK(f.cpu.gpr[4] == 0x1ef8); CHECK(f.get(0x1ef8) == (reg == 4 ? 0x1f00 : 0x1234));
        code.back() += 8; f.code(code); f.put(0x1ef8,0x1870);
        CHECK(step(f.cpu,f.memory).reason == stop_reason::completed); CHECK(f.cpu.gpr[reg] == 0x1870); CHECK(f.cpu.gpr[4] == (reg == 4 ? 0x1870 : 0x1f00));
    }
    f.code({0xe8,0x0b,0,0,0}); f.cpu.gpr[4] = 0x1f00;
    CHECK(step(f.cpu,f.memory).reason == stop_reason::completed); CHECK(f.cpu.rip == 0x1010); CHECK(f.get(0x1ef8) == 0x1005);
    f.store->bytes[16] = std::byte{0xc3}; CHECK(step(f.cpu,f.memory).reason == stop_reason::completed); CHECK(f.cpu.rip == 0x1005); CHECK(f.cpu.gpr[4] == 0x1f00);
}
void failures() {
    fixture f;
    for (auto code : {std::vector<unsigned>{0x66,0x90}, {0xf3,0x90}, {0x48,0x48,0x90}, {0x48,0x90}, {0x01,0x00}, {0x0f,0x0b}, {0xff,0xd0}, {0x83,0xd0,1}}) {
        f.code(code); const auto saved = f.cpu; CHECK(step(f.cpu,f.memory).reason == stop_reason::unsupported); CHECK(f.cpu == saved);
    }
    for (auto code : {std::vector<unsigned>{0xcc}, {0x0f,0x05}}) {
        f.code(code); const auto saved = f.cpu; CHECK(step(f.cpu,f.memory).reason == (code[0] == 0xcc ? stop_reason::breakpoint : stop_reason::syscall)); CHECK(f.cpu == saved);
    }
    f.cpu.rip = 0x1fff; f.store->bytes.back() = std::byte{0x48}; auto saved = f.cpu;
    auto result = step(f.cpu,f.memory); CHECK(result.reason == stop_reason::memory_failure); CHECK(result.access == access_kind::fetch); CHECK(f.cpu == saved);
    const std::array<std::vector<unsigned>,5> complete{{{0x48,0xb8,1,2,3,4,5,6,7,8},{0x48,0x8b,0x84,0x23,0,0,0,0},{0x48,0x81,0xc0,1,2,3,4},{0x0f,0x84,1,2,3,4},{0xe8,1,2,3,4}}};
    for (const auto& bytes : complete) for (std::size_t length = 1; length < bytes.size(); ++length) {
        f.cpu.rip = 0x2000 - length;
        for (std::size_t i = 0; i < length; ++i) f.store->bytes[4096 - length + i] = static_cast<std::byte>(bytes[i]);
        saved = f.cpu; result = step(f.cpu,f.memory);
        CHECK(result.reason == stop_reason::memory_failure); CHECK(result.access == access_kind::fetch); CHECK(result.fault_address == 0x2000); CHECK(result.length == length); CHECK(f.cpu == saved);
    }
    f.cpu.rip = 0x800000000000ULL; saved = f.cpu; CHECK(step(f.cpu,f.memory).reason == stop_reason::invalid_address); CHECK(f.cpu == saved);
    for (std::uint64_t address : {0x3000ULL,0x1ffcULL,0x800000000000ULL}) {
        f.code({0x48,0x8b,0x03}); f.cpu.gpr[3] = address; saved = f.cpu; result = step(f.cpu,f.memory);
        CHECK(result.reason == (address == 0x800000000000ULL ? stop_reason::invalid_address : stop_reason::memory_failure)); CHECK(f.cpu == saved);
    }
    f.code({0x48,0x89,0x03}); f.cpu.gpr[3] = 0x1800; f.cpu.gpr[0] = 0x55;
    CHECK(f.memory.protect(0x1000,guest_memory_access::read_only)); saved = f.cpu; result = step(f.cpu,f.memory);
    CHECK(result.reason == stop_reason::memory_failure); CHECK(result.memory_error == guest_memory_error::access_denied); CHECK(f.cpu == saved); CHECK(f.get(0x1800) == 0);
    CHECK(f.memory.protect(0x1000,guest_memory_access::read_write)); f.store->fail_write = true;
    result = step(f.cpu,f.memory); CHECK(result.reason == stop_reason::memory_failure); CHECK(result.memory_uncertain); CHECK(result.access == access_kind::write); CHECK(f.cpu == saved); CHECK(f.get(0x1800) == 0x55);
    f.store->fail_write = false; f.code({0xc3}); f.cpu.gpr[4] = 0x1800; f.put(0x1800,0x800000000000ULL); saved = f.cpu;
    CHECK(step(f.cpu,f.memory).reason == stop_reason::invalid_address); CHECK(f.cpu == saved);
    for (auto code : {std::vector<unsigned>{0x50},{0x58},{0xe8,0,0,0,0},{0xc3}}) {
        f.code(code); f.cpu.gpr[4] = 0x3000; saved = f.cpu;
        CHECK(step(f.cpu,f.memory).reason == stop_reason::memory_failure); CHECK(f.cpu == saved);
    }
    f.code({0x90}); CHECK(f.memory.protect(0x1000,guest_memory_access::none)); saved = f.cpu; result = step(f.cpu,f.memory);
    CHECK(result.reason == stop_reason::memory_failure); CHECK(result.access == access_kind::fetch); CHECK(result.memory_error == guest_memory_error::access_denied); CHECK(f.cpu == saved);
    CHECK(f.memory.protect(0x1000,guest_memory_access::read_write));
    f.code({0x90}); CHECK(step(f.cpu,f.memory).reason == stop_reason::completed); CHECK(f.cpu.rip == 0x1001);
}
void store_boundaries() {
    fixture f;
    auto adjacent = std::make_unique<backing>(); auto* adjacent_bytes = adjacent.get();
    std::unique_ptr<memory_backing> owned = std::move(adjacent);
    CHECK(f.memory.map(0x2000,4096,guest_memory_access::read_write,owned));
    f.code({0x48,0x89,0x03}); f.cpu.gpr[0] = 0xfedcba9876543210ULL; f.cpu.gpr[3] = 0x1ffc;
    const auto before = f.cpu; const auto bytes_before = f.store->bytes; const auto adjacent_before = adjacent_bytes->bytes;
    auto result = step(f.cpu,f.memory);
    CHECK(result.reason == stop_reason::memory_failure); CHECK(result.memory_error == guest_memory_error::invalid_range);
    CHECK(result.access == access_kind::write); CHECK(result.fault_address == 0x1ffc); CHECK(!result.wrote_memory);
    CHECK(f.cpu == before); CHECK(f.store->bytes == bytes_before); CHECK(adjacent_bytes->bytes == adjacent_before);

    auto edge = std::make_unique<backing>(); auto* edge_bytes = edge.get(); owned = std::move(edge);
    constexpr std::uint64_t edge_base = 0x7ffffffff000ULL;
    CHECK(f.memory.map(edge_base,4096,guest_memory_access::read_write,owned));
    f.cpu.gpr[3] = 0x7ffffffffffcULL; const auto canonical_before = f.cpu; const auto edge_before = edge_bytes->bytes;
    result = step(f.cpu,f.memory);
    CHECK(result.reason == stop_reason::invalid_address); CHECK(result.access == access_kind::write);
    CHECK(result.fault_address == 0x7ffffffffffcULL); CHECK(!result.wrote_memory); CHECK(!result.memory_uncertain);
    CHECK(f.cpu == canonical_before); CHECK(edge_bytes->bytes == edge_before); CHECK(f.store->bytes == bytes_before);

    // CALL itself fits in canonical mapped memory, but its relative target does
    // not. In particular, its return address must never reach the valid stack.
    constexpr std::array<unsigned,5> call{0xe8,0x20,0,0,0};
    for (std::size_t i = 0; i < call.size(); ++i) edge_bytes->bytes[0xff0 + i] = static_cast<std::byte>(call[i]);
    f.cpu.rip = edge_base + 0xff0; f.cpu.gpr[4] = 0x1f00;
    f.put(0x1ef8,0x1122334455667788ULL);
    const auto call_before = f.cpu; const auto stack_before = f.store->bytes; const auto code_before = edge_bytes->bytes;
    result = step(f.cpu,f.memory);
    CHECK(result.reason == stop_reason::invalid_address); CHECK(!result.wrote_memory); CHECK(!result.memory_uncertain);
    CHECK(f.cpu == call_before); CHECK(f.store->bytes == stack_before); CHECK(edge_bytes->bytes == code_before);
}
} // namespace
int main() { arithmetic(); moves(); branches(); failures(); store_boundaries(); std::printf("interpreter-conformance checks=%u failures=0\n",checks); }
