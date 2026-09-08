#include <pcsx5/execution/arm64_jit.h>
#include <pcsx5/execution/arm64_step.h>
#include <pcsx5/runtime/executable_code.h>
#include <algorithm>
#include <cstdio>
#include <memory>
#if !defined(__aarch64__) && !defined(_M_ARM64)
#error This test must execute on real ARM64, not an x64 compile-only gate.
#endif
namespace ex=pcsx5::execution;
namespace core=pcsx5::core;
namespace rt=pcsx5::runtime;
extern "C" void pcsx5_arm64_abi_probe(void(*)(void*),void*,std::uint64_t*,std::uint64_t*);
extern "C" void pcsx5_arm64_abi_corrupt(void*);
#if defined(_WIN32)
constexpr auto create_code=rt::create_windows_code;
#else
constexpr auto create_code=rt::create_posix_code;
#endif
namespace {
class image final : public core::memory_backing {
public:
    explicit image(std::span<const std::byte> code) { std::copy(code.begin(),code.end(),bytes.begin()); }
    std::uint64_t size() const noexcept override { return bytes.size(); }
    core::guest_memory_result<void> read(std::uint64_t offset,std::span<std::byte> output) const noexcept override {
        if (output.size()>bytes.size() || offset>bytes.size()-output.size()) return std::unexpected(core::guest_memory_error::invalid_range);
        std::copy_n(bytes.begin()+static_cast<std::size_t>(offset),output.size(),output.begin()); return {};
    }
    core::guest_memory_result<void> write(std::uint64_t offset,std::span<const std::byte> input) noexcept override {
        if (input.size()>bytes.size() || offset>bytes.size()-input.size()) return std::unexpected(core::guest_memory_error::invalid_range);
        std::copy(input.begin(),input.end(),bytes.begin()+static_cast<std::size_t>(offset)); return {};
    }
    core::guest_memory_result<void> release() noexcept override { return {}; }
    std::array<std::byte,4096> bytes{};
};
unsigned comparisons{},images{};
struct invocation { rt::executable_code* code; ex::cpu_state* state; bool success{}; };
void invoke_checked(void* opaque) {
    auto& call=*static_cast<invocation*>(opaque);
    call.success=call.code->invoke(call.state).has_value();
}
std::uint64_t random_state=0x9876543210abcdefULL;
std::uint64_t random_value() {
    random_state^=random_state<<13; random_state^=random_state>>7; random_state^=random_state<<17; return random_state;
}
constexpr std::array<std::uint64_t,8> edges{0,1,15,16,0x7fffffff,0x80000000,0x7fffffffffffffffULL,~0ULL};
bool check(std::span<const std::byte> bytes,unsigned repetitions=8) {
    const auto lowered=ex::lower_scalar(bytes,0x1000);
    if (!lowered) return false;
    const auto compiled=ex::emit_arm64(std::span(&*lowered,1));
    if (!compiled) return false;
    auto executable=create_code(*compiled,rt::code_isa::arm64);
    if (!executable) { std::fprintf(stderr,"code cache error=%d\n",static_cast<int>(executable.error())); return false; }
    ++images;
    core::guest_memory memory;
    std::unique_ptr<core::memory_backing> backing=std::make_unique<image>(bytes);
    if (!memory.map(0x1000,4096,core::guest_memory_access::read_only,backing)) return false;
    for (unsigned iteration=0;iteration<repetitions;++iteration) {
        struct frame { std::array<std::uint64_t,8> before; ex::cpu_state state; std::array<std::uint64_t,8> after; } actual{};
        actual.before.fill(0x123456789abcdef0ULL); actual.after.fill(0xfedcba9876543210ULL);
        for (auto& reg:actual.state.gpr) reg=random_value();
        actual.state.gpr[lowered->destination]=edges[iteration%edges.size()];
        if (!lowered->immediate && lowered->source!=lowered->destination)
            actual.state.gpr[lowered->source]=edges[(iteration+3)%edges.size()];
        actual.state.rip=0x1000;
        actual.state.rflags=2 | 0x400 | (random_value() & ex::arithmetic_flags);
        actual.state.known_flags=random_value() & ex::arithmetic_flags;
        const auto before=actual.before,after=actual.after;
        auto expected=actual.state;
        const auto result=ex::step(expected,memory);
        if (result.reason!=ex::stop_reason::completed || !(*executable)->invoke(&actual.state)) return false;
        ++comparisons;
        if (actual.state!=expected || actual.before!=before || actual.after!=after) {
            std::fprintf(stderr,"ARM64 mismatch image=%u case=%u op=%d width=%u dst=%u src=%u flags=%llx expected=%llx\n",
                images,comparisons,static_cast<int>(lowered->operation),lowered->width,lowered->destination,lowered->source,
                static_cast<unsigned long long>(actual.state.rflags),static_cast<unsigned long long>(expected.rflags)); return false;
        }
    }
    return true;
}
std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (auto value:values) result.push_back(static_cast<std::byte>(value)); return result;
}
bool hybrid() {
    // MOV/ADD compile; memory, stack, conditional branch and INT3 use the oracle.
    const auto program=bytes({0xb8,7,0,0,0,0x48,0x89,3,0x48,0x8b,0xb,0x50,0x5a,
        0x48,0x83,0xc1,1,0x48,0x39,0xc8,0x75,1,0x90,0xcc});
    core::guest_memory actual_memory,expected_memory;
    for (auto* memory : {&actual_memory,&expected_memory}) {
        std::unique_ptr<core::memory_backing> code=std::make_unique<image>(program);
        std::unique_ptr<core::memory_backing> data=std::make_unique<image>(std::span<const std::byte>{});
        if (!memory->map(0x1000,4096,core::guest_memory_access::read_only,code) ||
            !memory->map(0x3000,4096,core::guest_memory_access::read_write,data)) return false;
    }
    ex::cpu_state actual{},expected{};
    actual.rip=0x1000; actual.gpr[3]=0x3000; actual.gpr[4]=0x4000; expected=actual;
    unsigned translated{},fallback{};
    for (unsigned i=0;i<16;++i) {
        const auto oracle=ex::step(expected,expected_memory);
        const auto result=ex::step_arm64(actual,actual_memory,create_code);
        std::array<std::byte,4096> left{},right{};
        if (!result || result->result!=oracle || actual!=expected ||
            !actual_memory.read(0x3000,left) || !expected_memory.read(0x3000,right) || left!=right) {
            std::fprintf(stderr,"hybrid mismatch step=%u\n",i); return false;
        }
        result->translated ? ++translated : ++fallback;
        if (oracle.reason==ex::stop_reason::breakpoint) {
            if (translated!=3 || fallback!=6) return false;
            std::puts("physical ARM64 hybrid: 3 translated + 6 fallback steps, exact state/metadata/memory PASS");
            return true;
        }
    }
    return false;
}
rt::code_result<std::unique_ptr<rt::executable_code>> denied_code(std::span<const std::byte>,rt::code_isa) noexcept {
    return std::unexpected(rt::code_error::unavailable);
}
bool boundaries() {
    core::guest_memory memory;
    const auto program=bytes({0x90,0x0f,0x05,0x74,0,0x0f,0xff});
    std::unique_ptr<core::memory_backing> code=std::make_unique<image>(program);
    if (!memory.map(0x1000,4096,core::guest_memory_access::read_only,code)) return false;
    ex::cpu_state state{}; state.rip=0x1000;
    const auto before=state;
    const auto denied=ex::step_arm64(state,memory,denied_code);
    if (denied || denied.error()!=rt::code_error::unavailable || state!=before) return false;
    const auto invalid=ex::step_arm64(state,memory,nullptr);
    if (invalid || invalid.error()!=rt::code_error::invalid || state!=before) return false;
    // A provider that always fails must never be called for unsupported forms.
    for (auto rip : {0x1001ULL,0x1003ULL,0x1005ULL,0x2000ULL}) {
        state.rip=rip; state.known_flags=0;
        auto expected=state;
        const auto oracle=ex::step(expected,memory);
        const auto result=ex::step_arm64(state,memory,denied_code);
        if (!result || result->translated || result->result!=oracle || state!=expected ||
            oracle.reason==ex::stop_reason::completed) return false;
    }
    std::puts("physical ARM64 boundaries: provider failure, null provider, syscall, unknown flags, unsupported and fetch fault PASS");
    return true;
}
}
int main() {
    if (!boundaries()) return 1;
    if (!hybrid()) return 1;
    for (unsigned rex=0x40;rex<=0x4f;++rex) for (unsigned destination=0;destination<8;++destination)
        for (unsigned delta : {0U,3U}) for (unsigned opcode : {0x89U,0x8bU,1U,3U,9U,11U,0x21U,0x23U,0x29U,0x2bU,0x31U,0x33U,0x39U,0x3bU,0x85U}) {
            const auto source=(destination+delta)%8;
            if (!check(bytes({rex,opcode,0xc0U|(source<<3)|destination}))) return 1;
        }
    for (unsigned rex : {0x40U,0x41U,0x48U,0x49U}) for (unsigned destination=0;destination<8;++destination)
        for (unsigned group : {0U,1U,4U,5U,6U,7U}) for (unsigned immediate : {0U,0x7fU,0x80U,0xffU}) {
            if (!check(bytes({rex,0x83,0xc0U|(group<<3)|destination,immediate}))) return 1;
            if (!check(bytes({rex,0x81,0xc0U|(group<<3)|destination,immediate,0x56,0x34,0x80}))) return 1;
        }
    for (unsigned rex : {0x40U,0x41U,0x48U,0x49U}) for (unsigned destination=0;destination<8;++destination)
        for (auto immediate:edges) {
            auto code=bytes({rex,0xb8+destination});
            for (unsigned i=0;i<((rex&8)?8U:4U);++i) code.push_back(static_cast<std::byte>(immediate>>(i*8)));
            if (!check(code,2)) return 1;
        }
    if (!check(bytes({0x90}))) return 1;
    // Compile a whole straight-line block, not merely unrelated one-step calls.
    const auto block_bytes=bytes({0xb8,7,0,0,0,0xbb,9,0,0,0,0x48,1,0xd8,0x48,0x31,0xc9,0x48,0x83,0xe8,1});
    std::vector<ex::scalar_instruction> block;
    for (std::size_t offset=0;offset<block_bytes.size();) {
        const auto lowered=ex::lower_scalar(std::span(block_bytes).subspan(offset),0x1000+offset);
        if (!lowered) return 1;
        block.push_back(*lowered); offset+=lowered->length;
    }
    const auto compiled=ex::emit_arm64(block); if (!compiled) return 1;
    auto executable=create_code(*compiled,rt::code_isa::arm64); if (!executable) return 1;
    core::guest_memory memory;
    std::unique_ptr<core::memory_backing> backing=std::make_unique<image>(block_bytes);
    if (!memory.map(0x1000,4096,core::guest_memory_access::read_only,backing)) return 1;
    ex::cpu_state actual{},expected{}; actual.rip=expected.rip=0x1000;
    for (std::size_t i=0;i<block.size();++i) if (ex::step(expected,memory).reason!=ex::stop_reason::completed) return 1;
    if (!(*executable)->invoke(&actual) || actual!=expected) return 1;
    std::array<std::uint64_t,21> abi_before{},abi_after{};
    pcsx5_arm64_abi_probe(pcsx5_arm64_abi_corrupt,nullptr,abi_before.data(),abi_after.data());
    if (abi_before==abi_after || abi_before[0]==abi_after[0] || abi_before[12]==abi_after[12]) return 1;
    actual={}; actual.rip=0x1000;
    invocation call{executable->get(),&actual};
    pcsx5_arm64_abi_probe(invoke_checked,&call,abi_before.data(),abi_after.data());
    if (!call.success || actual!=expected || abi_before!=abi_after) return 1;
    std::puts("physical ARM64 ABI: X19-X29, SP, D8-D15, FPCR preserved; corruption negative control PASS");
    std::printf("physical ARM64: %u full-state comparisons, %u immutable images, reused code and multi-instruction block PASS\n",comparisons,images);
}
