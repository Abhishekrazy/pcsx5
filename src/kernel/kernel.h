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

    // Guest filesystem roots.  `SetApp0Directory` is called once with the
    // main module's directory; `SetSaveDataDirectory` with the host dir that
    // backs the save-data HLE (HLE::GetSaveDataDir()).  `TranslateGuestPath`
    // then maps guest paths onto those host directories:
    //   "/app0/..."      -> app0 dir
    //   "/savedata0/..." -> save-data dir
    //   "<rel>"          -> app0 dir (guest CWD is the package root)
    // Anything else (host-absolute paths, unmapped mounts) is returned
    // unchanged.
    void SetApp0Directory(const std::string& dir);
    void SetSaveDataDirectory(const std::string& dir);
    std::string TranslateGuestPath(const std::string& guest_path);

    // The process-wide module resolver (used by the HLE module-load path).
    Loader::ModuleResolver& GetModuleResolver();

    // Load and link a module (main executable or dynamic library)
    bool LoadModule(const std::string& filepath, Loader::LoadedModule& out_module);

    // Run the main module starting from its entry point.  Returns the guest's
    // exit code via `out_guest_exit_code` (0 when the guest simply returned).
    bool Execute(const Loader::LoadedModule& main_module, u32* out_guest_exit_code = nullptr);

    // Register a thread
    void RegisterThread(const ThreadContext& context);

    // Resolve the guest thread pointer for a guest thread id (per-thread
    // tls_base when registered, otherwise the shared TLS block — mirrors the
    // VEH fs-emulation lookup).
    guest_addr_t ResolveGuestThreadPointer(u64 guest_tid);

    // Resolve system calls (syscall instructions)
    void HandleSyscall(u32 syscall_number, guest_addr_t context);
}
// namespace Kernel
