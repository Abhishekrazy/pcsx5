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

    // In-process mode (core hosted inside the WPF app): process-wide host
    // hooks (top-level SEH filter, CRT death handlers) are left to the host
    // process, and the VEH only inspects exceptions raised while executing
    // tracked guest code so CLR first-chance exceptions pass through cheaply.
    // Must be called before Initialize().
    void SetInProcMode(bool enabled);

    // True when the core is hosted inside another process (see SetInProcMode).
    bool IsInProcMode();

    // Directory the VEH crash dumps (crash_rip_dump.bin, crash_prx_dump.bin)
    // are written to.  Set by the core once the crash directory is resolved,
    // so the dumps land beside the crash-report bundle rather than in the
    // process working directory.  Defaults to "pcsx5_crash" if never called.
    void SetCrashDumpDir(const std::string& dir);

    // Diagnostic data write-watchpoint, selected with PCSX5_WATCH_ADDR (hex,
    // 8-byte aligned).  Uses a hardware debug register rather than page
    // protection: it is exact to the qword, so unrelated writes to the same
    // page are not trapped, and it reports AFTER the store, so the value that
    // was written is visible.  Debug registers are per-thread state, so every
    // thread that runs guest code must arm itself.
    void ArmWatchpointForCurrentThread();

    // Periodic sampler for guest execution, enabled with PCSX5_SAMPLE_MS.
    // A frozen guest reports nothing: it is not faulting, not crashing and not
    // logging, so there is no way to see where it is spinning. This suspends
    // each guest thread briefly, records its RIP, and reports the hot addresses
    // at shutdown. Off unless the variable is set.
    void StartGuestSampler();
    void StopGuestSampler();

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

    // Dynamic-TLS (module-keyed) resolution for __tls_get_addr({ti_module,
    // ti_offset}).  `tp` is the caller's thread pointer.  Returns the guest
    // address of the TLS variable: for the main module (ti_module<=1) and for
    // any unknown module it falls back to `tp + ti_offset` (today's behavior,
    // byte-identical); for a known secondary module it serves a dedicated
    // per-thread TLS block seeded from that module's PT_TLS template.
    guest_addr_t ResolveDynamicTls(u64 guest_tid, u64 ti_module, u64 ti_offset,
                                   guest_addr_t tp);

    // Loaded-module registry used by sceKernelGetModuleInfo* HLE so a game
    // can map a code/data address back to its owning module (name, base,
    // segments).  RegisterLoadedModule is called during module linking;
    // FindModuleForAddr returns the module whose [base, base+image_size)
    // range contains `addr`, or null.
    void RegisterLoadedModule(const Loader::LoadedModule& module);
    const Loader::LoadedModule* FindModuleForAddr(guest_addr_t addr);
    
    // Returns the address of the PT_SCE_PROC_PARAM segment for the main executable
    guest_addr_t GetMainModuleProcessParam();

    // Resolve system calls (syscall instructions)
    void HandleSyscall(u32 syscall_number, guest_addr_t context);
}
// namespace Kernel
