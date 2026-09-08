#include "native_step_testing.h"
#include <cstdio>
#if defined(PCSX5_TEST_LINUX)
#include <cerrno>
#include <sys/wait.h>
#else
#define NOMINMAX
#include <windows.h>
#endif

namespace ex = pcsx5::execution;
namespace nt = pcsx5::execution::testing;
namespace {
ex::native_result inject(const std::filesystem::path& helper,const ex::native_request& request,
    std::uint32_t timeout,nt::native_failure_stage stage,bool* reached=nullptr) {
#if defined(PCSX5_TEST_LINUX)
    return nt::step_linux_native_injected(helper,request,timeout,stage,reached);
#else
    return nt::step_windows_native_injected(helper,request,timeout,stage,reached);
#endif
}
unsigned checks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(false)
int batch(const std::filesystem::path& helper) {
    ex::native_request request;
    request.initial.rip=ex::native_code_base;
    request.initial.gpr[4]=ex::native_data_base+2048;
    request.code[0]=std::byte{0x90}; // Authored NOP, followed by zero-initialized code.
    const auto expected=request.initial;
    // A successful control proves that this host/helper can reach all seams;
    // host_failure alone would otherwise permit a false pass before setup.
    auto control=inject(helper,request,5000,nt::native_failure_stage::none);
    CHECK(control.has_value());
    CHECK(control->stop==ex::native_stop::stepped && control->state.rip==expected.rip+1);
    for (const auto stage : {nt::native_failure_stage::after_images,
        nt::native_failure_stage::after_context,nt::native_failure_stage::before_snapshot}) {
        bool reached{};
        const auto failed=inject(helper,request,5000,stage,&reached);
        CHECK(reached && !failed && failed.error()==ex::native_error::host_failure);
        CHECK(request.initial==expected && request.code[0]==std::byte{0x90});
        // Repeat the last seam after a real fault event as well as a real step.
        auto fault=request; fault.code[0]=std::byte{0x0f}; fault.code[1]=std::byte{0x0b};
        reached=false;
        const auto fault_failed=inject(helper,fault,5000,stage,&reached);
        CHECK(reached && !fault_failed && fault_failed.error()==ex::native_error::host_failure);
        control=inject(helper,request,5000,nt::native_failure_stage::none);
        CHECK(control.has_value() && control->stop==ex::native_stop::stepped);
        auto state=expected; ++state.rip;
        CHECK(control->state==state && control->data==request.data);
    }
    bool held{};
    const auto timed=inject(helper,request,1000,nt::native_failure_stage::hold_after_context,&held);
    CHECK(held && !timed && timed.error()==ex::native_error::timed_out);
    for (const auto invalid : {-1,5,255}) {
        const auto failed=inject(helper / "missing",request,5000,static_cast<nt::native_failure_stage>(invalid));
        CHECK(!failed && failed.error()==ex::native_error::invalid);
    }
#if defined(PCSX5_TEST_LINUX)
    int status{}; errno=0;
    CHECK(waitpid(-1,&status,WNOHANG)==-1 && errno==ECHILD);
#endif
    return 0;
}
}
int main(int argc,char** argv) {
    CHECK(argc==2);
    const std::filesystem::path helper=argv[1];
    CHECK(batch(helper)==0); // Warm precisely the same paths that are measured.
#if !defined(PCSX5_TEST_LINUX)
    DWORD baseline{};
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&baseline)!=0);
#endif
    for (unsigned round=0; round<3; ++round) {
        CHECK(batch(helper)==0);
#if !defined(PCSX5_TEST_LINUX)
        DWORD observed{};
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&observed)!=0);
        if (observed!=baseline) std::fprintf(stderr,"failure handles round=%u baseline=%lu observed=%lu\n",round,baseline,observed);
        CHECK(observed==baseline);
#endif
    }
    std::printf("native-failure: real resources injected stages cleanup repeated PASS checks=%u\n",checks);
}
