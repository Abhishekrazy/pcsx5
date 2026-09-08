#include <pcsx5/execution/native_run.h>
#include <cstdio>
#include <initializer_list>

namespace ex = pcsx5::execution;
namespace {
#if defined(PCSX5_TEST_LINUX)
constexpr auto hardware_step = ex::step_linux_native;
#else
constexpr auto hardware_step = ex::step_windows_native;
#endif
unsigned checks{}, hardware_calls{};
bool corrupt_hardware_result{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(false)

ex::native_result counted_step(const std::filesystem::path& helper,
    const ex::native_request& request, std::uint32_t timeout) noexcept {
    ++hardware_calls;
    auto result=hardware_step(helper,request,timeout);
    // Deliberately corrupt an actual completed hardware observation; never
    // fabricate a successful native result to exercise the divergence path.
    if (result && corrupt_hardware_result) result->state.gpr[0] ^= 1;
    return result;
}
ex::native_run_request authored(std::initializer_list<unsigned> code) {
    ex::native_run_request request;
    request.budget=16;
    request.machine.initial.rip=ex::native_code_base;
    request.machine.initial.gpr[4]=ex::native_data_base+2048;
    request.machine.code.fill(std::byte{0xcc});
    std::size_t offset{};
    for (const auto byte : code) request.machine.code[offset++]=static_cast<std::byte>(byte);
    return request;
}
void store64(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned i=0; i<8; ++i) bytes[offset+i]=static_cast<std::byte>((value>>(i*8))&255);
}
bool chained(const ex::native_run_request& request, const ex::native_run_response& response) {
    if (response.records.empty()) return response.final==request.machine.initial;
    if (response.records.front().before!=request.machine.initial) return false;
    for (std::size_t i=1; i<response.records.size(); ++i) {
        const auto& previous=response.records[i-1].observed.state;
        const auto& before=response.records[i].before;
        if (before!=previous) return false;
    }
    const auto& last=response.records.back().observed;
    return response.final==last.state && response.data==last.data;
}
}
int main(int argc, char** argv) {
    CHECK(argc==2);
    const std::filesystem::path helper(argv[1]);
    auto request=authored({0xb8,7,0,0,0,0x83,0xc0,1,0xcc}); // MOV EAX,7; ADD EAX,1; INT3.
    auto response=ex::run_native(helper,request,counted_step,5000);
    CHECK(response.has_value()); CHECK(response->stop==ex::native_run_stop::requested_exit);
    CHECK(hardware_calls==3 && response->records.size()==3 && response->retired==2);
    auto expected=request.machine.initial; expected.gpr[0]=8; expected.rip+=9; expected.rflags=2;
    CHECK(response->final==expected && response->data==request.machine.data); CHECK(chained(request,*response));
    CHECK(response->records[0].observed.stop==ex::native_stop::stepped);
    CHECK(response->records[0].observed.state.gpr[0]==7 && response->records[0].observed.state.rip==ex::native_code_base+5);
    CHECK(response->records[1].observed.state.gpr[0]==8 && response->records[1].observed.state.rip==ex::native_code_base+8);
    CHECK(response->records[2].observed.stop==ex::native_stop::breakpoint);
    CHECK(response->records[2].preflight.reason==ex::stop_reason::breakpoint);

    request=authored({0x48,0xcc}); // The supported single REX prefix is part of INT3's length.
    hardware_calls=0; response=ex::run_native(helper,request,counted_step,5000);
    CHECK(response.has_value() && response->stop==ex::native_run_stop::requested_exit);
    CHECK(hardware_calls==1 && response->records.size()==1 && response->retired==0);
    CHECK(response->final.rip==ex::native_code_base+2 && response->records[0].preflight.length==2);

    request=authored({0x50,0xc3}); // PUSH caller's return boundary; RET.
    request.machine.initial.gpr[0]=request.return_address;
    hardware_calls=0; response=ex::run_native(helper,request,counted_step,5000);
    CHECK(response.has_value() && response->stop==ex::native_run_stop::returned);
    CHECK(hardware_calls==2 && response->records.size()==2 && response->retired==2);
    expected=request.machine.initial; expected.rip=request.return_address;
    auto image=request.machine.data; store64(image,2040,request.return_address);
    CHECK(response->final==expected && response->data==image); CHECK(chained(request,*response));
    CHECK(response->records[0].observed.state.gpr[4]==ex::native_data_base+2040);

    for (const bool bad_stack : {false,true}) {
        request=bad_stack ? authored({0x50}) : authored({0x48,0x8b,0x03});
        if (bad_stack) request.machine.initial.gpr[4]=0;
        hardware_calls=0; response=ex::run_native(helper,request,counted_step,5000);
        CHECK(response.has_value() && response->stop==ex::native_run_stop::guest_fault);
        CHECK(hardware_calls==1 && response->records.size()==1 && response->retired==0);
        CHECK(response->final==request.machine.initial && response->data==request.machine.data);
        CHECK(response->records[0].observed.stop==ex::native_stop::memory_fault);
        CHECK(response->records[0].preflight.reason==ex::stop_reason::memory_failure);
        CHECK(chained(request,*response));
    }

    request=authored({0xb8,7,0,0,0,0xcc}); request.budget=1;
    hardware_calls=0; response=ex::run_native(helper,request,counted_step,5000);
    CHECK(response.has_value() && response->stop==ex::native_run_stop::budget_exhausted);
    CHECK(hardware_calls==1 && response->records.size()==1 && response->retired==1);
    CHECK(response->final.gpr[0]==7 && response->final.rip==ex::native_code_base+5);
    request=authored({0xeb,0xfe}); request.budget=3;
    hardware_calls=0; response=ex::run_native(helper,request,counted_step,5000);
    CHECK(response.has_value() && response->stop==ex::native_run_stop::budget_exhausted);
    CHECK(hardware_calls==3 && response->retired==3 && response->records.size()==3);
    CHECK(response->final==request.machine.initial && response->data==request.machine.data);
    CHECK(chained(request,*response));

    for (unsigned kind=0; kind<3; ++kind) {
        request=kind==0 ? authored({0xf4}) : kind==1 ? authored({0x0f,0x05}) : authored({0x74,0});
        if (kind==2) request.machine.initial.known_flags=0;
        hardware_calls=0; response=ex::run_native(helper,request,counted_step,5000);
        CHECK(response.has_value() && response->stop==ex::native_run_stop::unsupported);
        CHECK(hardware_calls==0 && response->records.empty() && response->retired==0);
        CHECK(response->final==request.machine.initial && response->data==request.machine.data);
    }
    for (const auto opcode : {0x0fU,0xb8U}) {
        request=authored({0x90});
        request.machine.initial.rip=ex::native_code_base+ex::native_page_bytes-1;
        request.return_address=ex::native_code_base+128;
        request.machine.code.back()=static_cast<std::byte>(opcode);
        hardware_calls=0; response=ex::run_native(helper,request,counted_step,5000);
        CHECK(response.has_value() && response->stop==ex::native_run_stop::unsupported);
        CHECK(hardware_calls==0 && response->records.empty() && response->retired==0);
        CHECK(response->final==request.machine.initial && response->data==request.machine.data);
    }
    request=authored({0x90}); request.machine.initial.rip=ex::native_data_base;
    request.machine.data[0]=std::byte{0x90}; // Readable data must not become an executable whitelist.
    hardware_calls=0; response=ex::run_native(helper,request,counted_step,5000);
    CHECK(response.has_value() && response->stop==ex::native_run_stop::unsupported);
    CHECK(hardware_calls==0 && response->records.empty() && response->retired==0);
    CHECK(response->final==request.machine.initial && response->data==request.machine.data);
    request=authored({0x31,0xc0,0xcc}); // XOR EAX,EAX: AF is undefined.
    request.machine.initial.gpr[0]=~std::uint64_t{};
    hardware_calls=0; response=ex::run_native(helper,request,counted_step,5000);
    CHECK(response.has_value() && response->stop==ex::native_run_stop::requested_exit);
    CHECK(hardware_calls==2 && response->retired==1 && response->records.size()==2);
    CHECK(response->final.gpr[0]==0 && response->final.rip==ex::native_code_base+3);
    CHECK((response->final.known_flags & 0x10)==0);
    CHECK(response->records[0].observed.state.known_flags==(ex::arithmetic_flags & ~0x10ULL));
    CHECK(response->records[1].before==response->records[0].observed.state);
    CHECK((response->final.rflags & ~0x10ULL)==0x46); CHECK(chained(request,*response));

    request=authored({0xb8,7,0,0,0});
    corrupt_hardware_result=true; hardware_calls=0;
    response=ex::run_native(helper,request,counted_step,5000); corrupt_hardware_result=false;
    CHECK(response.has_value() && response->stop==ex::native_run_stop::divergence);
    CHECK(hardware_calls==1 && response->records.size()==1);
    CHECK(response->records[0].observed.state.gpr[0]==6 && response->final.gpr[0]==6);
    CHECK(chained(request,*response)); // Must not replace observed 6 with oracle 7.

    hardware_calls=0; request=authored({0x90});
    CHECK(!ex::run_native(helper,request,nullptr,5000));
    request.budget=0; CHECK(!ex::run_native(helper,request,counted_step,5000));
    request.budget=257; CHECK(!ex::run_native(helper,request,counted_step,5000));
    request.budget=1; request.return_address=ex::native_code_base-1;
    CHECK(!ex::run_native(helper,request,counted_step,5000));
    request.return_address=ex::native_code_base+ex::native_page_bytes;
    CHECK(!ex::run_native(helper,request,counted_step,5000));
    request=authored({0x90}); CHECK(!ex::run_native(helper,request,counted_step,0));
    CHECK(!ex::run_native(helper,request,counted_step,60001)); CHECK(hardware_calls==0);
    request.return_address=request.machine.initial.rip;
    response=ex::run_native(helper,request,counted_step,5000);
    CHECK(response.has_value() && response->stop==ex::native_run_stop::returned);
    CHECK(response->records.empty() && response->retired==0 && hardware_calls==0);
    CHECK(response->final==request.machine.initial && response->data==request.machine.data);
    std::printf("native-run: actual state exit return faults budgets whitelist divergence PASS checks=%u\n",checks);
}
