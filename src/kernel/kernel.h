#pragma once
#include "../common/types.h"
#include "../loader/elf.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace Kernel {

    struct ThreadContext {
        u64 thread_id = 0;
        std::string name;
        guest_addr_t entry_point = 0;
        guest_addr_t stack_base = 0;
        u64 stack_size = 0;
    };

    bool Initialize();
    void Shutdown();

    // Load and link a module (main executable or dynamic library)
    bool LoadModule(const std::string& filepath, Loader::LoadedModule& out_module);

    // Run the main module starting from its entry point
    bool Execute(const Loader::LoadedModule& main_module);

    // Register a thread
    void RegisterThread(const ThreadContext& context);

    // Resolve system calls (syscall instructions)
    void HandleSyscall(u32 syscall_number, guest_addr_t context);
}
// namespace Kernel
