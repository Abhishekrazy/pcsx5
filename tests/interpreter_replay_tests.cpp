#include <pcsx5/execution/interpreter.h>
#include <pcsx5/runtime/guest_memory_backing.h>
#include "host_memory_provider.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>
namespace ex = pcsx5::execution;
namespace core = pcsx5::core;
namespace rt = pcsx5::runtime;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return false; } } while(false)
// Newly authored bytes; no compiler, assembler, legacy fixture or retail input.
constexpr std::array<unsigned char,45> program{
    0x48,0xb8,0x10,0,0,0,0,0,0,0, // mov rax,16
    0xb9,3,0,0,0,                 // mov ecx,3
    0x48,0x01,0xc8,               // add rax,rcx
    0x83,0xe9,1,                  // sub ecx,1
    0x75,0xf8,                    // jne loop
    0x48,0x89,0x03,               // mov [rbx],rax
    0x48,0x8b,0x13,               // mov rdx,[rbx]
    0x52,0x41,0x58,               // push rdx; pop r8
    0xe8,1,0,0,0,                 // call subroutine
    0xcc,                        // pre-execution breakpoint stop
    0x41,0xb9,0x7b,0,0,0,0xc3};   // mov r9d,123; ret
constexpr std::size_t record_count = 19;
using corpus = std::array<ex::trace_record,record_count>;
using wire_corpus = std::array<std::byte,record_count * ex::trace_wire_size>;
ex::cpu_state initial_state() {
    ex::cpu_state state{};
    state.rip = 0x1000; state.gpr[3] = 0x3000; state.gpr[4] = 0x3800;
    return state;
}
corpus expected_trace() {
    corpus expected{};
    auto state = initial_state();
    std::size_t index{};
    auto append = [&](ex::cpu_state next, std::size_t length, std::uint64_t write_address = 0,
                      std::uint64_t write_value = 0, bool stopped = false) {
        auto& rec = expected[index];
        rec.sequence = index++; rec.before = state; rec.after = next;
        rec.result.length = static_cast<std::uint8_t>(length);
        for (std::size_t i = 0; i < length; ++i)
            rec.result.instruction[i] = static_cast<std::byte>(program[static_cast<std::size_t>(state.rip-0x1000)+i]);
        if (write_address != 0) {
            rec.result.wrote_memory = true; rec.result.write_address = write_address; rec.result.write_size = 8;
            for (unsigned i = 0; i < 8; ++i) rec.result.write_bytes[i] = static_cast<std::byte>((write_value >> (8*i)) & 255);
        }
        if (stopped) rec.result.reason = ex::stop_reason::breakpoint;
        state = next;
    };
    auto next = state; next.gpr[0] = 16; next.rip += 10; append(next,10);
    next.gpr[1] = 3; next.rip += 5; append(next,5);
    // Hand-calculated full states: ADD results 19,21,22; SUB results 2,1,0.
    for (const auto total : {19U,21U,22U}) {
        next.gpr[0] = total; next.rip = 0x1012; next.rflags = 2; append(next,3);
        --next.gpr[1]; next.rip = 0x1015; next.rflags = next.gpr[1] == 0 ? 0x46 : 2; append(next,3);
        next.rip = next.gpr[1] == 0 ? 0x1017 : 0x100f; append(next,2);
    }
    next.rip = 0x101a; append(next,3,0x3000,22);
    next.gpr[2] = 22; next.rip = 0x101d; append(next,3);
    next.gpr[4] = 0x37f8; next.rip = 0x101e; append(next,1,0x37f8,22);
    next.gpr[8] = 22; next.gpr[4] = 0x3800; next.rip = 0x1020; append(next,2);
    next.gpr[4] = 0x37f8; next.rip = 0x1026; append(next,5,0x37f8,0x1025);
    next.gpr[9] = 123; next.rip = 0x102c; append(next,6);
    next.gpr[4] = 0x3800; next.rip = 0x1025; append(next,1);
    append(next,1,0,0,true);
    return expected;
}
bool replay(wire_corpus& output) {
    auto code = rt::make_guest_memory_backing(4096, test_host::reserve_memory);
    auto data = rt::make_guest_memory_backing(4096, test_host::reserve_memory);
    CHECK(code && data);
    core::guest_memory memory;
    CHECK(memory.map(0x1000,4096,core::guest_memory_access::read_write,*code));
    CHECK(memory.map(0x3000,4096,core::guest_memory_access::read_write,*data));
    CHECK(memory.write(0x1000,std::as_bytes(std::span(program))));
    CHECK(memory.protect(0x1000,core::guest_memory_access::read_only));
    const auto expected = expected_trace();
    corpus actual{};
    auto state = initial_state();
    const auto capacity = ex::run(state,memory,100,std::span(actual).first(2));
    CHECK(capacity.reason == ex::stop_reason::trace_full && capacity.records == 2 && capacity.retired == 2);
    CHECK(state == expected[1].after && actual[0] == expected[0] && actual[1] == expected[1]);
    state = initial_state();
    const auto result = ex::run(state,memory,100,actual);
    CHECK(result.reason == ex::stop_reason::breakpoint && result.retired == 18 && result.records == record_count);
    for (std::size_t i = 0; i < record_count; ++i) {
        if (actual[i] != expected[i]) { std::fprintf(stderr,"record %zu differs from independent oracle\n",i); return false; }
        auto bytes = std::span(output).subspan(i*ex::trace_wire_size,ex::trace_wire_size);
        CHECK(ex::encode_trace(actual[i],bytes));
        ex::trace_record decoded{}; CHECK(ex::decode_trace(bytes,decoded)); CHECK(decoded == expected[i]);
        // Valid structural mutations remain distinguishable by oracle equality.
        for (auto& reg : decoded.after.gpr) { reg ^= 1; CHECK(decoded != expected[i]); reg ^= 1; }
    }
    std::array<std::byte,8> value{};
    CHECK(memory.read(0x3000,value)); CHECK(value[0] == std::byte{22});
    CHECK(std::all_of(value.begin()+1,value.end(),[](auto b){return b == std::byte{};}));
    CHECK(memory.read(0x37f8,value));
    CHECK((value == std::array<std::byte,8>{std::byte{0x25},std::byte{0x10}}));
    // Resume in bounded chunks with identical per-step results and state. Reset
    // memory to the original image rather than reusing residual stack/data.
    std::array<std::byte,4096> zero{}; CHECK(memory.write(0x3000,zero));
    state = initial_state();
    for (std::size_t i = 0; i < record_count; ++i) {
        std::array<ex::trace_record,1> single{};
        const auto part = ex::run(state,memory,1,single);
        CHECK(part.records == 1);
        CHECK(part.reason == (i+1 == record_count ? ex::stop_reason::breakpoint : ex::stop_reason::budget_exhausted));
        single[0].sequence = i; CHECK(single[0] == expected[i]);
    }
    CHECK(memory.unmap(0x1000)); CHECK(memory.unmap(0x3000));
    return true;
}
int main(int argc, char** argv) {
    wire_corpus first{}, second{};
    if (!replay(first) || !replay(second) || first != second) return 1;
    if (argc == 2 && std::string_view(argv[1]) == "--trace") {
        std::fputs("PXI1:",stdout);
        for (const auto byte : first) std::printf("%02x",std::to_integer<unsigned>(byte));
        std::putchar('\n');
    } else if (argc == 1) {
        std::puts("interpreter-replay-v1: records=19 retired=18 full-state memory stack branches fresh resume wire PASS");
    } else return 2;
}
