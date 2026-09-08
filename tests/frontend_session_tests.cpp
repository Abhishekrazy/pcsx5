#include <pcsx5/frontend/session.h>
#include <cstdio>
namespace front=pcsx5::frontend;
namespace ex=pcsx5::execution;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(false)
int main() {
    const std::array code{std::byte{0xb8},std::byte{7},std::byte{},std::byte{},std::byte{},
        std::byte{0x83},std::byte{0xc0},std::byte{9},std::byte{0xcc}};
    CHECK(!front::run_program({},1));
    CHECK(!front::run_program(code,0)); CHECK(!front::run_program(code,1'000'001));
    std::array<std::byte,4097> too_large{}; CHECK(!front::run_program(too_large,1));
    const auto result=front::run_program(code,10);
    CHECK(result && result->state.gpr[0]==16 && result->retired==2 && result->interpreted==2 &&
        result->translated==0 && result->stop==ex::stop_reason::breakpoint);
    const auto bounded=front::run_program(code,1);
    CHECK(bounded && bounded->retired==1 && bounded->state.gpr[0]==7 && bounded->stop==ex::stop_reason::budget_exhausted);
    const std::array syscall{std::byte{0x0f},std::byte{0x05}};
    const auto denied=front::run_program(syscall,5);
    CHECK(denied && denied->retired==0 && denied->stop==ex::stop_reason::syscall);
    const std::array loop{std::byte{0xeb},std::byte{0xfe}};
    const auto looping=front::run_program(loop,100);
    CHECK(looping && looping->retired==100 && looping->state.rip==0x1000 && looping->stop==ex::stop_reason::budget_exhausted);
    const auto provider=[](std::span<const std::byte>,pcsx5::runtime::code_isa) noexcept
        ->pcsx5::runtime::code_result<std::unique_ptr<pcsx5::runtime::executable_code>> {
        return std::unexpected(pcsx5::runtime::code_error::unavailable);
    };
    auto failed=front::run_program(code,10,provider);
    CHECK(!failed && failed.error()==front::session_error::runtime_failure);
    std::puts("frontend session: bounds, result, budget, syscall stop and provider failure PASS");
}
