#include "process_test_signal.h"
#include <array>
#include <chrono>
#include <string_view>
#include <thread>
#if defined(PCSX5_TEST_LINUX)
#include <csignal>
#include <sys/resource.h>
#endif
int main(int argc, char** argv) {
#if !defined(PCSX5_TEST_LINUX)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    if (argc < 2) return 90;
    const std::string_view mode = argv[1];
    if (mode == "exit") return 7;
    if (mode == "args") {
        constexpr std::array<std::string_view,6> expected{"","space value","quote\"value","trailing\\","a\\\"b","&|<>$()"};
        if (argc != 8) return 91;
        for (std::size_t i=0; i<expected.size(); ++i) if (argv[i+2] != expected[i]) return 92;
        return 23;
    }
    if (mode == "fault") {
#if defined(PCSX5_TEST_LINUX)
        const rlimit no_core{0,0};
        if (setrlimit(RLIMIT_CORE,&no_core) != 0) return 93;
        std::raise(SIGSEGV);
#else
        RaiseException(EXCEPTION_ACCESS_VIOLATION,EXCEPTION_NONCONTINUABLE,0,nullptr);
#endif
        return 94;
    }
    if (mode == "ready") {
        if (argc != 3 || !process_test_signal::notify(argv[2])) return 95;
    } else if (mode != "hang") return 96;
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1)); // Deliberately stalled child, not test synchronization.
}
