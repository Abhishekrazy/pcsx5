#include <pcsx5/execution/native_step.h>
#include <array>
#include <cstdio>
#include <future>
#include <initializer_list>
#if defined(PCSX5_TEST_LINUX)
#include <cerrno>
#include <sys/wait.h>
#else
#define NOMINMAX
#include <windows.h>
#endif

namespace ex = pcsx5::execution;
namespace {
#if defined(PCSX5_TEST_LINUX)
constexpr auto native_step = ex::step_linux_native;
#else
constexpr auto native_step = ex::step_windows_native;
#endif
unsigned checks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(false)
ex::native_request authored(std::initializer_list<unsigned> code) {
    ex::native_request q;
    q.code.fill(std::byte{0xcc});
    std::size_t offset{};
    for (const auto byte : code) q.code[offset++]=static_cast<std::byte>(byte);
    for (std::size_t reg=0; reg<16; ++reg) q.initial.gpr[reg]=0x1234567800000000ULL+reg*0x101;
    q.initial.gpr[4]=ex::native_data_base+2048;
    q.initial.rip=ex::native_code_base;
    q.initial.rflags=2;
    for (std::size_t i=0; i<q.data.size(); ++i) q.data[i]=static_cast<std::byte>(i&255);
    return q;
}
void store64(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned i=0; i<8; ++i) bytes[offset+i]=static_cast<std::byte>((value>>(i*8))&255);
}
}
int run_batch(const std::filesystem::path& helper) {
    // These are ordinary parent locals, not an assembly-level ABI proof. The
    // hardware instructions execute only inside the disposable child.
    const std::array<std::uint64_t,4> parent_sentinel{0x1234,0x5678,0x9abc,0xdef0};
    auto q=authored({0xb8,7,0,0,0}); // MOV EAX,7 zero-extends RAX.
    auto expected=q.initial; expected.gpr[0]=7; expected.rip+=5;
    auto result=native_step(helper,q,5000);
    if (!result) std::fprintf(stderr,"initial native step error=%d\n",static_cast<int>(result.error()));
    CHECK(result.has_value());
    CHECK(result->stop==ex::native_stop::stepped); CHECK(result->state==expected);
    CHECK(result->data==q.data && result->fault_address==0);

    q=authored({0x48,0x01,0xd8}); // ADD RAX,RBX: unsigned wrap, no signed overflow.
    q.initial.gpr[0]=~std::uint64_t{}; q.initial.gpr[3]=1;
    expected=q.initial; expected.gpr[0]=0; expected.rip+=3; expected.rflags=0x57;
    result=native_step(helper,q,5000); CHECK(result.has_value()); CHECK(result->stop==ex::native_stop::stepped);
    CHECK(result->state==expected); CHECK(result->data==q.data);

    q=authored({0x48,0x89,0x03}); // MOV [RBX],RAX.
    q.initial.gpr[3]=ex::native_data_base+64; expected=q.initial; expected.rip+=3;
    auto image=q.data; store64(image,64,q.initial.gpr[0]);
    result=native_step(helper,q,5000); CHECK(result.has_value()); CHECK(result->state==expected);
    CHECK(result->stop==ex::native_stop::stepped && result->data==image);
    q=authored({0x48,0x8b,0x03}); // MOV RAX,[RBX].
    q.initial.gpr[3]=ex::native_data_base+64; store64(q.data,64,0x8070605040302010ULL);
    expected=q.initial; expected.gpr[0]=0x8070605040302010ULL; expected.rip+=3;
    result=native_step(helper,q,5000); CHECK(result.has_value()); CHECK(result->state==expected);
    CHECK(result->stop==ex::native_stop::stepped && result->data==q.data);

    q=authored({0x50}); // PUSH RAX.
    expected=q.initial; expected.gpr[4]-=8; ++expected.rip;
    image=q.data; store64(image,2040,q.initial.gpr[0]);
    result=native_step(helper,q,5000); CHECK(result.has_value()); CHECK(result->state==expected);
    CHECK(result->stop==ex::native_stop::stepped && result->data==image);
    q=authored({0x58}); // POP RAX.
    store64(q.data,2048,0xfedcba9876543210ULL); expected=q.initial;
    expected.gpr[0]=0xfedcba9876543210ULL; expected.gpr[4]+=8; ++expected.rip;
    result=native_step(helper,q,5000); CHECK(result.has_value()); CHECK(result->state==expected);
    CHECK(result->stop==ex::native_stop::stepped && result->data==q.data);
    q=authored({0xc3}); // RET reaches an owned code address, before its INT3 executes.
    store64(q.data,2048,ex::native_code_base+128); expected=q.initial;
    expected.gpr[4]+=8; expected.rip=ex::native_code_base+128;
    result=native_step(helper,q,5000); CHECK(result.has_value()); CHECK(result->state==expected);
    CHECK(result->stop==ex::native_stop::stepped && result->data==q.data);

    q=authored({0xcc}); expected=q.initial; ++expected.rip;
    result=native_step(helper,q,5000); CHECK(result.has_value()); CHECK(result->stop==ex::native_stop::breakpoint);
    CHECK(result->state==expected); CHECK(result->data==q.data);
    q=authored({0x0f,0x0b}); // UD2: precise illegal-instruction stop.
    result=native_step(helper,q,5000); CHECK(result.has_value()); CHECK(result->stop==ex::native_stop::illegal_instruction);
    CHECK(result->state==q.initial && result->data==q.data);

    q=authored({0x48,0x8b,0x03}); q.initial.gpr[3]=0;
    result=native_step(helper,q,5000); CHECK(result.has_value()); CHECK(result->stop==ex::native_stop::memory_fault);
    CHECK(result->state==q.initial && result->data==q.data && result->fault_address==0);
    q=authored({0x48,0x89,0x03}); q.initial.gpr[3]=ex::native_code_base;
    result=native_step(helper,q,5000); CHECK(result.has_value()); CHECK(result->stop==ex::native_stop::memory_fault);
    // The snapshot does not expose code bytes: this proves write protection via
    // the observed precise fault, not a separate code-image comparison.
    CHECK(result->state==q.initial && result->data==q.data && result->fault_address==ex::native_code_base);
    q=authored({0x90}); q.initial.rip=ex::native_data_base; q.data[0]=std::byte{0x90};
    result=native_step(helper,q,5000); CHECK(result.has_value()); CHECK(result->stop==ex::native_stop::memory_fault);
    CHECK(result->state==q.initial && result->data==q.data && result->fault_address==ex::native_data_base);
    q=authored({0x50}); q.initial.gpr[4]=8; // PUSH would write address zero.
    result=native_step(helper,q,5000); CHECK(result.has_value()); CHECK(result->stop==ex::native_stop::memory_fault);
    CHECK(result->state==q.initial && result->data==q.data && result->fault_address==0);
    q=authored({0xc3}); q.initial.gpr[4]=0;
    result=native_step(helper,q,5000); CHECK(result.has_value()); CHECK(result->stop==ex::native_stop::memory_fault);
    CHECK(result->state==q.initial && result->data==q.data && result->fault_address==0);

    q=authored({0x90}); q.initial.rflags|=0x100; // Caller cannot import TF.
    result=native_step(helper,q,5000); CHECK(!result && result.error()==ex::native_error::invalid);
    q=authored({0x90}); q.initial.known_flags|=0x100;
    result=native_step(helper,q,5000); CHECK(!result && result.error()==ex::native_error::invalid);
    q=authored({0x90});
    result=native_step(helper,q,0); CHECK(!result && result.error()==ex::native_error::invalid);
    result=native_step(helper,q,60001); CHECK(!result && result.error()==ex::native_error::invalid);
    result=native_step({},q,5000); CHECK(!result);
    result=native_step(helper / "nonexistent-child",q,5000); CHECK(!result);

    for (unsigned repetition=0; repetition<3; ++repetition) {
        q=authored({0x90}); expected=q.initial; ++expected.rip;
        result=native_step(helper,q,5000); CHECK(result.has_value()); CHECK(result->state==expected);
        q=authored({0x0f,0x0b}); result=native_step(helper,q,5000);
        CHECK(result.has_value()); CHECK(result->stop==ex::native_stop::illegal_instruction && result->state==q.initial);
    }
    const auto good=authored({0x90}), bad=authored({0x0f,0x0b});
    auto first=std::async(std::launch::async,[&]{ return native_step(helper,good,5000); });
    auto second=std::async(std::launch::async,[&]{ return native_step(helper,bad,5000); });
    const auto first_result=first.get(), second_result=second.get();
    expected=good.initial; ++expected.rip;
    CHECK(first_result.has_value() && first_result->stop==ex::native_stop::stepped && first_result->state==expected);
    CHECK(second_result.has_value() && second_result->stop==ex::native_stop::illegal_instruction && second_result->state==bad.initial);
    CHECK((parent_sentinel==std::array<std::uint64_t,4>{0x1234,0x5678,0x9abc,0xdef0}));
#if defined(PCSX5_TEST_LINUX)
    int child_status{}; errno=0;
    CHECK(waitpid(-1,&child_status,WNOHANG)==-1 && errno==ECHILD);
#endif
    return 0;
}
int main(int argc,char** argv) {
    CHECK(argc==2);
    const std::filesystem::path helper=argv[1];
    // Warm the exact success, fault, missing-helper and concurrent call paths.
    // Windows loader/async first-use caches are not per-invocation resource leaks.
    CHECK(run_batch(helper)==0);
#if !defined(PCSX5_TEST_LINUX)
    DWORD initial_handles{};
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&initial_handles)!=0);
#endif
    for (unsigned round=0; round<3; ++round) {
        CHECK(run_batch(helper)==0);
#if !defined(PCSX5_TEST_LINUX)
        DWORD final_handles{};
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&final_handles)!=0);
        if (final_handles!=initial_handles)
            std::fprintf(stderr,"handle round=%u baseline=%lu observed=%lu\n",round,initial_handles,final_handles);
        CHECK(final_handles==initial_handles);
#endif
    }
    std::printf("native-step: hardware state memory stack traps faults repeated concurrent PASS checks=%u\n",checks);
}
