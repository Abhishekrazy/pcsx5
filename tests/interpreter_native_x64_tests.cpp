#include <pcsx5/execution/interpreter.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>

#if !defined(__linux__) || !defined(__x86_64__) || !(defined(__GNUC__) || defined(__clang__))
#error This native oracle requires Linux x64 and GNU-compatible inline assembly.
#endif

using namespace pcsx5::core;
using namespace pcsx5::execution;
namespace {
unsigned checks{};
unsigned cases{};
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "native oracle line %d case %u: %s\n", __LINE__, cases, #expression); \
    std::exit(1); } } while (false)

struct native_result { std::uint64_t value{}, flags{}; };

// These are fixed compiler-assembled instructions, never executable guest data.
// The target MUST use -mno-red-zone: PUSHFQ temporarily writes below RSP.
// Early-clobber constraints prevent either output overlapping the source input;
// flags capture is in the same asm statement with no intervening flag changes.
#define NATIVE_CASE(number, instruction, suffix, operand) \
    case number: \
        __asm__ volatile(instruction suffix " %" operand "[right], %" operand "[left]\n\t" \
            "pushfq\n\tpopq %[flags]" \
            : [left] "+&r"(out.value), [flags] "=&r"(out.flags) \
            : [right] "r"(right) : "cc", "memory"); \
        break

native_result native_alu(unsigned operation, std::uint64_t left,
    std::uint64_t right, bool wide) noexcept {
    native_result out{left, 0};
    if (wide) {
        switch (operation) {
        NATIVE_CASE(0, "add", "q", "q");
        NATIVE_CASE(1, "sub", "q", "q");
        NATIVE_CASE(2, "cmp", "q", "q");
        NATIVE_CASE(3, "xor", "q", "q");
        NATIVE_CASE(4, "and", "q", "q");
        NATIVE_CASE(5, "or", "q", "q");
        NATIVE_CASE(6, "test", "q", "q");
        default: std::abort();
        }
    } else {
        switch (operation) {
        NATIVE_CASE(0, "add", "l", "k");
        NATIVE_CASE(1, "sub", "l", "k");
        NATIVE_CASE(2, "cmp", "l", "k");
        NATIVE_CASE(3, "xor", "l", "k");
        NATIVE_CASE(4, "and", "l", "k");
        NATIVE_CASE(5, "or", "l", "k");
        NATIVE_CASE(6, "test", "l", "k");
        default: std::abort();
        }
    }
    return out;
}
#undef NATIVE_CASE

struct code_backing final : memory_backing {
    std::array<std::byte, 16> code{};
    std::uint64_t size() const noexcept override { return code.size(); }
    guest_memory_result<void> read(std::uint64_t offset, std::span<std::byte> out) const noexcept override {
        std::copy_n(code.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
        return {};
    }
    guest_memory_result<void> write(std::uint64_t, std::span<const std::byte>) noexcept override {
        return std::unexpected(guest_memory_error::access_denied);
    }
    guest_memory_result<void> release() noexcept override { return {}; }
};

void compare(guest_memory& memory, code_backing& backing, unsigned operation,
    std::uint64_t left, std::uint64_t right, bool wide) {
    constexpr std::array<unsigned, 7> opcodes{0x01, 0x29, 0x39, 0x31, 0x21, 0x09, 0x85};
    std::size_t length{};
    if (wide) backing.code[length++] = std::byte{0x48};
    backing.code[length++] = static_cast<std::byte>(opcodes[operation]);
    backing.code[length++] = std::byte{0xc8}; // Destination RAX/EAX, source RCX/ECX.
    cpu_state cpu{};
    cpu.rip = 0x1000;
    cpu.gpr[0] = left;
    cpu.gpr[1] = right;
    cpu.rflags = 0x202 | arithmetic_flags;
    const auto expected = native_alu(operation, left, right, wide);
    const auto result = step(cpu, memory);
    ++cases;
    const auto mask = operation < 3 ? arithmetic_flags : arithmetic_flags & ~std::uint64_t{0x10};
    CHECK(result.reason == stop_reason::completed);
    CHECK(result.length == length);
    CHECK(cpu.rip == 0x1000 + length);
    // Reading the complete 64-bit asm output also verifies EAX zero-extension;
    // CMP/TEST intentionally preserve the original complete register value.
    CHECK(cpu.gpr[0] == expected.value);
    CHECK(cpu.gpr[1] == right);
    CHECK((cpu.rflags & mask) == (expected.flags & mask));
    CHECK(cpu.known_flags == mask);
    CHECK(!result.wrote_memory);
}

std::uint64_t random_value(std::uint64_t& state) noexcept {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}
} // namespace

int main() {
    auto owned = std::make_unique<code_backing>();
    auto& backing = *owned;
    std::unique_ptr<memory_backing> base = std::move(owned);
    guest_memory memory;
    CHECK(memory.map(0x1000, 16, guest_memory_access::read_only, base));
    constexpr std::array<std::uint64_t, 14> edges{
        0, 1, 15, 16, 127, 128, 255, 0x7fffffff, 0x80000000,
        0xffffffff, 0x100000000, 0x7fffffffffffffffULL,
        0x8000000000000000ULL, 0xffffffffffffffffULL};
    for (const bool wide : {false, true}) {
        for (unsigned operation = 0; operation < 7; ++operation) {
            for (const auto left : edges)
                for (const auto right : edges)
                    compare(memory, backing, operation, left, right, wide);
            std::uint64_t seed = 0x92f13b571c4a608dULL;
            for (unsigned i = 0; i < 1024; ++i) {
                const auto left = random_value(seed);
                const auto right = random_value(seed);
                compare(memory, backing, operation, left, right, wide);
            }
        }
    }
    std::printf("interpreter-native-x64 cases=%u checks=%u failures=0\n", cases, checks);
}
