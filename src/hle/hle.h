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

    bool Initialize();
    void Shutdown();

    // Register an HLE function handler for a library symbol
    void RegisterSymbol(const std::string& module_name, const std::string& name, HleHandler handler);

    // Resolve symbol by exact module+name key; returns thunk address or 0
    guest_addr_t Resolve(const std::string& module_name, const std::string& name);

    // Resolve by NID name alone, searching all registered modules.
    // Falls back to auto-stub creation if not found anywhere.
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
