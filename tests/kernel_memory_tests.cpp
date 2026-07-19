// Kernel guest-memory tests — exact host page-protection mapping in
// Kernel::ProtectGuestMemory (src/kernel/memory.cpp).

#include "kernel/memory.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>

namespace {

int g_failures = 0;
int g_checks   = 0;

#define EXPECT(cond, msg)                                                            \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg);    \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

// Protect a fresh RW allocation through ProtectGuestMemory and verify the
// exact resulting PAGE_* value via VirtualQuery.
void CheckProtection(int guest_prot, DWORD expected_win_prot, const char* name) {
    void* mem = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    EXPECT(mem != nullptr, "VirtualAlloc scratch buffer");
    if (!mem) return;

    EXPECT(Kernel::ProtectGuestMemory(reinterpret_cast<guest_addr_t>(mem), 0x10000, guest_prot),
           name);

    MEMORY_BASIC_INFORMATION mbi{};
    EXPECT(VirtualQuery(mem, &mbi, sizeof(mbi)) != 0, "VirtualQuery after ProtectGuestMemory");
    if (mbi.Protect != expected_win_prot) {
        std::fprintf(stderr, "[FAIL] %s: expected PAGE_* 0x%lX, got 0x%lX\n",
                     name, expected_win_prot, mbi.Protect);
        ++g_failures;
    }
    ++g_checks;

    VirtualFree(mem, 0, MEM_RELEASE);
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    using namespace Kernel;
    CheckProtection(PROT_NONE,                        PAGE_NOACCESS,          "NONE -> PAGE_NOACCESS");
    CheckProtection(PROT_READ,                        PAGE_READONLY,          "R -> PAGE_READONLY");
    CheckProtection(PROT_WRITE,                       PAGE_READWRITE,         "W -> PAGE_READWRITE");
    CheckProtection(PROT_READ | PROT_WRITE,           PAGE_READWRITE,         "RW -> PAGE_READWRITE");
    CheckProtection(PROT_EXEC,                        PAGE_EXECUTE,           "X -> PAGE_EXECUTE");
    CheckProtection(PROT_READ | PROT_EXEC,            PAGE_EXECUTE_READ,      "RX -> PAGE_EXECUTE_READ");
    CheckProtection(PROT_WRITE | PROT_EXEC,           PAGE_EXECUTE_READWRITE, "WX -> PAGE_EXECUTE_READWRITE");
    CheckProtection(PROT_READ | PROT_WRITE | PROT_EXEC, PAGE_EXECUTE_READWRITE, "RWX -> PAGE_EXECUTE_READWRITE");

    std::fprintf(stdout, "Kernel memory: %d check(s), %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::fprintf(stdout, "Kernel memory tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "Kernel memory tests FAILED with %d failure(s).\n", g_failures);
    return 1;
}
