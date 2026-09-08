#include "native_step_testing.h"
#include <cstdio>
#include <cstddef>
namespace ex=pcsx5::execution;
namespace nt=ex::testing;
struct alignas(16) host_snapshot {
    std::array<std::uint64_t,8> gpr{};
    std::uint64_t rsp{};
    std::uint32_t mxcsr{};
    std::uint16_t control{},padding{};
    std::uint64_t flags{},padding2{};
    std::array<std::array<std::byte,16>,10> xmm{};
};
static_assert(sizeof(host_snapshot)==256 && offsetof(host_snapshot,rsp)==64 &&
    offsetof(host_snapshot,mxcsr)==72 && offsetof(host_snapshot,control)==76 &&
    offsetof(host_snapshot,flags)==80 && offsetof(host_snapshot,xmm)==96);
extern "C" void pcsx5_host_probe(void (*)(void*),void*,host_snapshot*,host_snapshot*);
extern "C" void pcsx5_host_corrupt(void*);
bool preserved(const host_snapshot& before,const host_snapshot& after) {
    return before.gpr==after.gpr && before.rsp==after.rsp && before.control==after.control &&
        ((before.mxcsr^after.mxcsr)&0xffc0U)==0 && (after.flags&0x400)==0 && before.xmm==after.xmm;
}
struct invocation {
    std::filesystem::path helper;
    ex::native_request request{};
    ex::native_result result=std::unexpected(ex::native_error::invalid);
    bool timeout{},reached{};
};
void invoke(void* opaque) noexcept {
    auto& call=*static_cast<invocation*>(opaque);
    const auto stage=call.timeout ? nt::native_failure_stage::hold_after_context : nt::native_failure_stage::none;
#if defined(PCSX5_TEST_LINUX)
    call.result=nt::step_linux_native_injected(call.helper,call.request,call.timeout?1000:5000,stage,&call.reached);
#else
    call.result=nt::step_windows_native_injected(call.helper,call.request,call.timeout?1000:5000,stage,&call.reached);
#endif
}
int main(int argc,char** argv) {
    if (argc!=2) return 1;
    // Deliberate RBX/DF corruption must be detected; assembly restores the
    // caller's original state even for this negative control.
    host_snapshot before{},after{};
    pcsx5_host_probe(pcsx5_host_corrupt,nullptr,&before,&after);
    if (preserved(before,after) || before.gpr==after.gpr || before.control==after.control ||
        ((before.mxcsr^after.mxcsr)&0xffc0U)==0 || (after.flags&0x400)==0) return 1;
#if !defined(PCSX5_TEST_LINUX)
    if (before.xmm==after.xmm) return 1;
#endif
    invocation call; call.helper=argv[1];
    for (unsigned mode=0;mode<6;++mode) {
        call.request={}; call.request.initial.rip=ex::native_code_base;
        call.request.initial.gpr[4]=ex::native_data_base+2048;
        call.request.code[0]=std::byte{0x90}; call.timeout=mode==5;
        if (mode==1) call.request.code[0]=std::byte{0xcc};
        if (mode==2) { call.request.code[0]=std::byte{0x0f}; call.request.code[1]=std::byte{0x0b}; }
        if (mode==3) { call.request.code[0]=std::byte{0x48}; call.request.code[1]=std::byte{0x8b}; call.request.code[2]=std::byte{3}; }
        if (mode==4) { call.request.code[0]=std::byte{0xc3}; call.request.initial.gpr[4]=0; }
        before={}; after={}; pcsx5_host_probe(invoke,&call,&before,&after);
        if (!preserved(before,after) || (call.timeout ?
            (!call.reached || call.result || call.result.error()!=ex::native_error::timed_out) : !call.result)) {
            std::fprintf(stderr,"host ABI mode %u failed\n",mode); return 1;
        }
    }
    std::puts("host ABI: actual nonvolatile GPRs/RSP/FP-control/DF (+Windows XMM6-15), negative control and native exit/fault/timeout PASS");
}
