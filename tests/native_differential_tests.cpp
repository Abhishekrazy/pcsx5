#include <pcsx5/execution/native_step.h>
#include <algorithm>
#include <cstdio>
#include <memory>
namespace ex = pcsx5::execution;
namespace core = pcsx5::core;
#if defined(PCSX5_TEST_LINUX)
constexpr auto native_step = ex::step_linux_native;
#else
constexpr auto native_step = ex::step_windows_native;
#endif
namespace {
class backing final : public core::memory_backing {
public:
    explicit backing(const std::array<std::byte,ex::native_page_bytes>& value) : bytes(value) {}
    std::uint64_t size() const noexcept override { return bytes.size(); }
    core::guest_memory_result<void> read(std::uint64_t offset,std::span<std::byte> out) const noexcept override {
        if (out.size()>bytes.size() || offset>bytes.size()-out.size()) return std::unexpected(core::guest_memory_error::invalid_range);
        std::copy_n(bytes.begin()+static_cast<std::size_t>(offset),out.size(),out.begin()); return {};
    }
    core::guest_memory_result<void> write(std::uint64_t offset,std::span<const std::byte> in) noexcept override {
        if (in.size()>bytes.size() || offset>bytes.size()-in.size()) return std::unexpected(core::guest_memory_error::invalid_range);
        std::copy(in.begin(),in.end(),bytes.begin()+static_cast<std::size_t>(offset)); return {};
    }
    core::guest_memory_result<void> release() noexcept override { return {}; }
    std::array<std::byte,ex::native_page_bytes> bytes;
};
unsigned cases{};
bool compare(const std::filesystem::path& helper,ex::native_request& request) {
    ++cases;
    core::guest_memory memory;
    std::unique_ptr<core::memory_backing> code=std::make_unique<backing>(request.code);
    std::unique_ptr<core::memory_backing> data=std::make_unique<backing>(request.data);
    if (!memory.map(ex::native_code_base,ex::native_page_bytes,core::guest_memory_access::read_only,code) ||
        !memory.map(ex::native_data_base,ex::native_page_bytes,core::guest_memory_access::read_write,data)) return false;
    auto expected=request.initial;
    const auto result=ex::step(expected,memory);
    if (result.reason!=ex::stop_reason::completed) return false;
    auto expected_data=request.data;
    if (!memory.read(ex::native_data_base,expected_data)) return false;
    const auto actual=native_step(helper,request,5000);
    if (!actual || actual->stop!=ex::native_stop::stepped || actual->state.gpr!=expected.gpr ||
        actual->state.rip!=expected.rip || actual->data!=expected_data ||
        ((actual->state.rflags^expected.rflags)&expected.known_flags)!=0) {
        std::fprintf(stderr,"native differential case %u failed (native error=%d)\n",cases,actual ? -1 : static_cast<int>(actual.error()));
        return false;
    }
    // Continue a sequence from actual hardware state, with undefined flags masked
    // according to the oracle contract. Never replace observed registers/memory.
    request.initial=actual->state;
    request.initial.known_flags=expected.known_flags;
    request.initial.rflags &= expected.known_flags | 2;
    request.data=actual->data;
    return true;
}
ex::native_request seed() {
    ex::native_request request{};
    for (std::size_t i=0;i<16;++i) request.initial.gpr[i]=0x123456789abcdef0ULL+i;
    request.initial.gpr[4]=ex::native_data_base+2048;
    request.initial.rip=ex::native_code_base;
    return request;
}
void instruction(ex::native_request& request,std::initializer_list<unsigned> bytes) {
    request.code.fill(std::byte{});
    std::size_t offset{};
    for (auto byte:bytes) request.code[offset++]=static_cast<std::byte>(byte);
}
}
int main(int argc,char** argv) {
    if (argc!=2) return 1;
    const std::filesystem::path helper(argv[1]);
    constexpr std::array<std::uint64_t,8> values{0,1,0xf,0x10,0x7fffffff,0x80000000,0x7fffffffffffffffULL,~0ULL};
    // Register-register arithmetic/logical instructions, both operand widths.
    for (const auto opcode : {0x01U,0x29U,0x39U,0x31U,0x21U,0x09U,0x85U})
        for (const bool wide : {false,true}) for (std::size_t i=0;i<values.size();++i) {
            auto request=seed();
            request.initial.gpr[0]=values[i]; request.initial.gpr[3]=values[(i+3)%values.size()];
            request.initial.rflags=ex::arithmetic_flags | 2;
            if (wide) instruction(request,{0x48,opcode,0xd8}); else instruction(request,{opcode,0xd8});
            if (!compare(helper,request)) return 1;
        }
    for (unsigned condition=0;condition<16;++condition) for (const auto flags : {2ULL,ex::arithmetic_flags|2ULL}) {
        auto request=seed(); request.initial.rflags=flags;
        instruction(request,{0x70+condition,4});
        if (!compare(helper,request)) return 1;
    }
    // A real multi-instruction state chain: move, memory, arithmetic, stack,
    // call, callee increment, return. Each hardware step has a fresh child owner.
    auto request=seed(); request.initial.gpr[3]=ex::native_data_base+128;
    instruction(request,{0xb8,7,0,0,0, 0x89,0x03, 0x8b,0x0b, 0x48,0x01,0xc8,
        0x50,0x5a, 0xe8,1,0,0,0, 0xcc, 0x48,0x83,0xc0,1, 0xc3});
    for (unsigned i=0;i<9;++i) if (!compare(helper,request)) return 1;
    if (request.initial.rip!=ex::native_code_base+19 || request.initial.gpr[0]!=15 ||
        request.initial.gpr[2]!=14 || request.initial.gpr[4]!=ex::native_data_base+2048) return 1;
    std::printf("native differential: %u full modeled-state/data comparisons PASS\n",cases);
}
