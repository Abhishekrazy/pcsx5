#include <pcsx5/execution/native_run.h>
#include <pcsx5/execution/worker_protocol.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
namespace ex = pcsx5::execution;
#if defined(PCSX5_NATIVE_LINUX)
constexpr auto provider=ex::step_linux_native;
#else
constexpr auto provider=ex::step_windows_native;
#endif
int main(int argc,char** argv) {
    if (argc!=2) return 1;
    const std::filesystem::path helper(argv[1]);
    ex::native_run_request native{};
    native.budget=24; native.machine.initial.rip=ex::native_code_base;
    // ADD RAX,1; JMP -6. Authored scalar loop, not a game benchmark.
    constexpr std::array code{0x48U,0x83U,0xc0U,1U,0xebU,0xfaU};
    for (std::size_t i=0;i<code.size();++i) native.machine.code[i]=static_cast<std::byte>(code[i]);
    ex::worker_request oracle{};
    oracle.id=1; oracle.budget=24; oracle.initial.rip=ex::worker_memory_base;
    for (std::size_t i=0;i<code.size();++i) oracle.memory[i]=static_cast<std::byte>(code[i]);
    std::array<long long,7> native_ns{},oracle_ns{};
    for (std::size_t sample=0;sample<8;++sample) {
        const auto start=std::chrono::steady_clock::now();
        const auto result=ex::run_native(helper,native,provider,5000);
        const auto end=std::chrono::steady_clock::now();
        if (!result || result->stop!=ex::native_run_stop::budget_exhausted ||
            result->retired!=24 || result->final.gpr[0]!=12) return 1;
        const auto oracle_start=std::chrono::steady_clock::now();
        for (unsigned repeat=0;repeat<1000;++repeat) {
            const auto expected=ex::execute_request(oracle);
            if (!expected || expected->result.retired!=24 || expected->final.gpr[0]!=12) return 1;
        }
        const auto oracle_end=std::chrono::steady_clock::now();
        if (sample!=0) {
            native_ns[sample-1]=std::chrono::duration_cast<std::chrono::nanoseconds>(end-start).count();
            oracle_ns[sample-1]=std::chrono::duration_cast<std::chrono::nanoseconds>(oracle_end-oracle_start).count()/1000;
        }
    }
    std::sort(native_ns.begin(),native_ns.end()); std::sort(oracle_ns.begin(),oracle_ns.end());
    std::printf("scalar-loop24,median_ns,min_ns,max_ns\n");
    std::printf("fresh-child-native,%lld,%lld,%lld\n",native_ns[3],native_ns.front(),native_ns.back());
    std::printf("interpreter-worker,%lld,%lld,%lld\n",oracle_ns[3],oracle_ns.front(),oracle_ns.back());
    std::puts("7 samples after warmup; native includes 24 child launches/debug/preflight/cleanup; interpreter includes worker trace validation. Not raw ISA throughput or PS5 performance.");
}
