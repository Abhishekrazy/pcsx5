// Original synthetic test. No default syscall table, OS syscall or trap invoked.
#define NOMINMAX
#include "kernel/syscalls.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {
int failures{};
unsigned calls{};
CONTEXT* expected_context{};
void Check(bool condition, const char* label) {
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}
s64 SyntheticHandler(CONTEXT* context) {
    ++calls;
    Check(context == expected_context, "original context identity reaches handler");
    const std::array actual{context->Rdi, context->Rsi, context->Rdx,
                            context->R10, context->R8, context->R9};
    for (std::size_t i = 0; i != actual.size(); ++i)
        Check(actual[i] == 0x101 + i, "six syscall argument slots remain intact");
    Check(context->Rcx == 0x777, "RCX remains distinct from fourth syscall argument R10");
    return -123;
}
}

int main() {
    // Scope the opt-in debugger hook out of this isolated process.
    if (_putenv_s("PCSX5_BREAK_SYSCALL", "") != 0) return 1;
    CONTEXT context;
    std::memset(&context, 0x5a, sizeof(context));
    context.Rax = 0x888;
    context.Rdi = 0x101; context.Rsi = 0x102; context.Rdx = 0x103;
    context.R10 = 0x104; context.R8 = 0x105; context.R9 = 0x106;
    context.Rcx = 0x777;
    std::array<unsigned char, sizeof(CONTEXT)> before{};
    std::memcpy(before.data(), &context, sizeof(context));
    expected_context = &context;
    Kernel::RegisterSyscallHandler(511, SyntheticHandler);
    Check(Kernel::HandleSyscall(511, &context) == -123, "registered handler signed return");
    Check(calls == 1, "registered handler invoked once");
    Check(std::memcmp(before.data(), &context, sizeof(context)) == 0,
          "raw dispatcher preserves context (RAX assignment belongs to outer trap handler)");
    Kernel::RegisterSyscallHandler(511, nullptr);
    Check(Kernel::HandleSyscall(511, &context) == -38, "cleared handler returns legacy ENOSYS");
    Check(Kernel::HandleSyscall(0, &context) == -38, "unregistered lower-bound entry");
    Kernel::RegisterSyscallHandler(512, SyntheticHandler);
    Check(Kernel::HandleSyscall(512, &context) == -38, "out-of-range table entry");
    Check(Kernel::HandleSyscall(std::numeric_limits<u32>::max(), &context) == -38,
          "maximum unsigned syscall number is rejected");
    Check(calls == 1 && std::memcmp(before.data(), &context, sizeof(context)) == 0,
          "rejected calls neither invoke handler nor mutate context");
    std::puts("Syscall table characterization only: real breakpoint/return ABI is not exercised");
    return failures ? 1 : 0;
}
