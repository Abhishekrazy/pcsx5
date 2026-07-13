#pragma once
#include "../common/types.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>

namespace HLE {

    // Structure containing registers passed from the guest application (System V ABI)
    struct GuestArgs {
        u64 arg1; // rdi
        u64 arg2; // rsi
        u64 arg3; // rdx
        u64 arg4; // rcx
        u64 arg5; // r8
        u64 arg6; // r9
    };

    using HleHandler = std::function<u64(const GuestArgs& args)>;

    struct HleSymbol {
        std::string module_name;
        std::string name;
        u64 id = 0;
        guest_addr_t thunk_address = 0;
        HleHandler handler;
    };

    // Aggregate statistics for a single resolved import — used by the test harness
    // and the compatibility database to make import resolution actionable.
    struct ImportStats {
        std::string module_name;
        std::string name;          // The NID string the guest actually requested
        guest_addr_t thunk_address = 0;
        u64 call_count = 0;        // Number of times the symbol has been dispatched
        guest_addr_t last_caller_rip = 0;  // RIP of the most recent guest caller
        u64 total_caller_samples = 0;       // Number of distinct RIPs observed
    };

    bool Initialize();
    void Shutdown();

    // Test-mode helpers --------------------------------------------------------
    // When strict-import mode is enabled, Resolve/ResolveAny refuse to auto-stub
    // unresolved NIDs. They return 0 and the kernel linker treats the relocation
    // as a hard error. This is required by the Phase-0 test suite.
    void SetStrictImportMode(bool enabled);
    bool IsStrictImportMode();

    // Returns a snapshot of every symbol that was actually invoked (call_count > 0)
    // along with module name, NID, call count, last caller RIP, and the thunk VA.
    std::vector<ImportStats> GetImportReport();

    // Returns the count of symbols that have been auto-stubbed because they were
    // requested by the guest but never registered.
    u64 GetUnresolvedImportCount();

    // Resets per-run counters so a single process can host multiple guest runs.
    void ResetRunStatistics();

    // Register an HLE function handler for a library symbol
    void RegisterSymbol(const std::string& module_name, const std::string& name, HleHandler handler);

    // Resolve symbol by exact module+name key; returns thunk address or 0
    guest_addr_t Resolve(const std::string& module_name, const std::string& name);

    // Resolve by NID name alone, searching all registered modules.
    // Falls back to auto-stub creation if not found anywhere (unless strict mode).
    guest_addr_t ResolveAny(const std::string& name);

    // Store the guest VA of the game's main() function (called from ELF loader)
    void SetGuestMainAddress(guest_addr_t addr);
    guest_addr_t GetGuestMainAddress();

    // Store the VA of the DT_INIT function (global ctor runner) for re-invocation if needed
    void SetDtInitAddress(guest_addr_t addr);
    guest_addr_t GetDtInitAddress();

    // Dynamic dispatcher callback (called by assembly bridge)
    extern "C" u64 HleDispatch(u64 symbol_id, u64 rdi, u64 rsi, u64 rdx, u64 rcx, u64 r8, u64 r9, u64 guest_rip);
}
// namespace HLE

// Assembly-language trampoline: call a guest function from within a Windows-ABI C++ function.
// Translates Windows ABI → System V ABI and updates g_host_stack_pointer.
// rcx = guest_func_va, rdx = rdi_arg, r8 = rsi_arg, r9 = rdx_arg
// Returns: rax = guest return value
extern "C" u64 InvokeGuestFunction(u64 guest_func_va, u64 rdi_arg, u64 rsi_arg, u64 rdx_arg);
