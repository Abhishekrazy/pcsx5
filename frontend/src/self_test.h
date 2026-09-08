#pragma once
#include <pcsx5/frontend/session.h>
namespace pcsx5::frontend {
inline execution::code_factory host_code_factory() noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
#if defined(__APPLE__)
    return runtime::create_darwin_code;
#elif defined(_WIN32)
    return runtime::create_windows_code;
#else
    return runtime::create_posix_code;
#endif
#else
    return nullptr;
#endif
}
inline bool self_test() noexcept {
    constexpr std::array code{std::byte{0xb8},std::byte{7},std::byte{},std::byte{},std::byte{},
        std::byte{0x83},std::byte{0xc0},std::byte{9},std::byte{0xcc}};
    const auto oracle=run_program(code,10);
    if (!oracle || oracle->state.gpr[0]!=16 || oracle->retired!=2 ||
        oracle->stop!=execution::stop_reason::breakpoint) return false;
    if (const auto factory=host_code_factory()) {
        const auto translated=run_program(code,10,factory);
        return translated && translated->state==oracle->state && translated->stop==oracle->stop &&
            translated->translated==2 && translated->retired==2;
    }
    return true;
}
}
