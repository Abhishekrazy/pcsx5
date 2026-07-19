#pragma once
#include "../common/types.h"
#include "../loader/elf.h"
#include "../loader/module_resolver.h"
#include "tls.h"
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
        guest_addr_t tls_base = 0;
        u64 argument = 0;  // SysV rdi arg passed to guest entry function
    };

    bool Initialize();
    void Shutdown();

    // Configure PRX module resolution.  `game_dir` is the directory of the
    // main module (its `sce_module/` sub-directory is searched first);
    // `firmware_modules_dir` (may be empty) holds user-supplied firmware
    // PRX/SPRX dumps and is searched second.  Modules that resolve to no
    // file continue to be served by HLE.
    void ConfigureModuleResolver(const std::string& game_dir,
                                 const std::string& firmware_modules_dir);

    // The process-wide module resolver (used by the HLE module-load path).
    Loader::ModuleResolver& GetModuleResolver();

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
