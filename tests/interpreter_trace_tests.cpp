#include <pcsx5/execution/interpreter.h>
#include <array>
#include <cstdio>
namespace ex = pcsx5::execution;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while(false)
int main() {
    ex::trace_record record{};
    record.before.rip = 0x1000;
    record.after = record.before;
    ++record.after.rip;
    record.result.length = 1;
    record.result.instruction[0] = std::byte{0x90};
    std::array<std::byte, ex::trace_wire_size> wire{};
    CHECK(ex::encode_trace(record, wire));
    // Independently laid out NOP record, including fixed header, scalar offsets,
    // known-flags mask and the canonical unused memory-error value.
    std::array<std::byte, ex::trace_wire_size> expected{};
    expected[0] = std::byte{'P'}; expected[1] = std::byte{'X'};
    expected[2] = std::byte{'I'}; expected[3] = std::byte{'1'};
    expected[141] = std::byte{0x10}; expected[148] = std::byte{2};
    expected[156] = std::byte{0xd5}; expected[157] = std::byte{8};
    expected[292] = std::byte{1}; expected[293] = std::byte{0x10};
    expected[300] = std::byte{2}; expected[308] = std::byte{0xd5}; expected[309] = std::byte{8};
    expected[318] = std::byte{1}; expected[328] = std::byte{0x90};
    expected[343] = std::byte{1};
    CHECK(wire == expected);
    ex::trace_record decoded{};
    CHECK(ex::decode_trace(wire, decoded)); CHECK(decoded == record);
    CHECK(!ex::decode_trace(std::span(wire).first(wire.size()-1), decoded));
    for (const auto offset : {0U, 3U, 316U, 317U, 318U, 327U, 343U, 344U, 361U}) {
        auto corrupt = wire; corrupt[offset] = std::byte{255};
        CHECK(!ex::decode_trace(corrupt, decoded)); CHECK(decoded == record);
    }
    auto corrupt = wire; corrupt[329] = std::byte{1};
    CHECK(!ex::decode_trace(corrupt, decoded)); // unused instruction bytes
    auto invalid = record; invalid.result.reason = ex::stop_reason::unsupported;
    CHECK(!ex::encode_trace(invalid, wire)); CHECK(wire == expected);
    std::array<ex::trace_record,1> records{};
    pcsx5::core::guest_memory empty;
    auto state = record.before;
    const auto zero = ex::run(state, empty, 0, records);
    CHECK(zero.reason == ex::stop_reason::budget_exhausted && zero.records == 0 && zero.retired == 0);
    CHECK(state == record.before);
    const auto full = ex::run(state, empty, 1, {});
    CHECK(full.reason == ex::stop_reason::trace_full && full.records == 0);
    const auto fault = ex::run(state, empty, 1, records);
    CHECK(fault.reason == ex::stop_reason::memory_failure && fault.records == 1 && fault.retired == 0);
    CHECK(records[0].before == state && records[0].after == state);
    CHECK(ex::encode_trace(records[0], wire)); CHECK(ex::decode_trace(wire, decoded));
    CHECK(decoded == records[0]);
    invalid = records[0]; invalid.result.memory_error = static_cast<pcsx5::core::guest_memory_error>(-1);
    const auto saved = wire;
    CHECK(!ex::encode_trace(invalid,wire)); CHECK(wire == saved);
    std::puts("interpreter-trace: wire golden malformed transactional bounded-run PASS");
}
