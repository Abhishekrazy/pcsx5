#pragma once
#include "../common/types.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <csetjmp>

namespace HLE {

    // Structure containing registers passed from the guest application (System V ABI)
    struct GuestArgs {
        u64 arg1; // rdi
        u64 arg2; // rsi
        u64 arg3; // rdx
        u64 arg4; // rcx
        u64 arg5; // r8
        u64 arg6; // r9
        // Guest address of the first stack-resident argument (SysV overflow
        // arg area): [guest_rsp_at_thunk_entry + 8].  Variadic HLE handlers
        // (sprintf family) read overflow varargs from here.  0 if the
        // dispatcher did not capture the guest stack pointer.
        u64 stack_args = 0;
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
        std::string resolved_name; // Friendly name if the NID is known, else == name
        guest_addr_t thunk_address = 0;
        u64 call_count = 0;        // Number of times the symbol has been dispatched
        guest_addr_t last_caller_rip = 0;  // RIP of the most recent guest caller
        u64 total_caller_samples = 0;       // Number of distinct RIPs observed
        bool auto_stubbed = false; // True if the symbol was auto-stubbed (unimplemented)
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

    // Returns the count of symbols that have been auto-stubbed because they
    // were requested by the guest but never registered.
    u64 GetUnresolvedImportCount();

    // Resets per-run counters so a single process can host multiple guest runs.
    void ResetRunStatistics();

    // Serializes the import report as a JSON array sorted by call_count
    // descending.  Each entry: {module, nid, name, call_count, auto_stubbed,
    // last_caller_rip} with last_caller_rip as a hex string.
    std::string ExportImportReportJson();

    // Writes ExportImportReportJson() to `path`.  Returns false on I/O error.
    bool WriteImportReportJson(const std::string& path);

    // Logs a one-line structured WARN (module + NID + resolved name) the first
    // time an unimplemented stub is invoked; subsequent calls stay silent —
    // hit counts remain visible via the import report / JSON export.
    void LogStubCallOnce(const std::string& module_name, const std::string& name);

    // Import-call trace.  A bounded ring of the most recent guest->host calls,
    // captured by the dispatcher for inclusion in crash reports.
    struct TraceEntry {
        u64                timestamp_us = 0;
        std::string        module_name;
        std::string        name;
        u64                symbol_id     = 0;
        guest_addr_t       caller_rip    = 0;
        guest_addr_t       thunk_address = 0;
        u64                arg1 = 0, arg2 = 0, arg3 = 0;
        u64                arg4 = 0, arg5 = 0, arg6 = 0;
    };

    // Returns the most recent N trace entries (chronological order).
    std::vector<TraceEntry> GetImportTrace(size_t max_count = 256);

    // Manually clear the trace ring.  Useful between guest runs.
    void ClearImportTrace();

    // Register an HLE function handler for a library symbol
    void RegisterSymbol(const std::string& module_name, const std::string& name, HleHandler handler);

    // Registers log-and-return-0 stubs (under their real names) for every NID
    // database entry that no HLE module implemented.  Called automatically at
    // the end of Initialize(); exposed so tests can re-run it after loading
    // an additional NID database file.  Never overrides an existing
    // registration (existing keys are skipped before RegisterSymbol runs).
    void RegisterNidDbStubs();

    // Resolve symbol by exact module+name key; returns thunk address or 0
    guest_addr_t Resolve(const std::string& module_name, const std::string& name);

    // Resolve by NID name alone, searching all registered modules.
    // Falls back to auto-stub creation if not found anywhere (unless strict mode).
    guest_addr_t ResolveAny(const std::string& name);

    // Store the guest VA of the game's main() function (called from ELF loader)
    void SetGuestMainAddress(guest_addr_t addr);
    guest_addr_t GetGuestMainAddress();

    // Tell the HLE C++ exception unwinder (liblibc.cpp) where the main
    // module's .eh_frame_hdr lives in guest memory.  Called from main() after
    // LoadModule with LoadedModule::eh_frame_hdr_addr; 0 disables unwinding
    // (__cxa_throw then logs and survives instead of unwinding).
    void SetGuestEhFrameHdr(guest_addr_t addr, u64 size);

    // Store the VA of the DT_INIT function (global ctor runner) for re-invocation if needed
    void SetDtInitAddress(guest_addr_t addr);
    guest_addr_t GetDtInitAddress();

    // Save-data host backing directory (libsavedata.cpp).  The title id is
    // supplied by main() from --title-id; GetSaveDataDir() creates and returns
    // <cwd>/pcsx5_savedata/<title-id>/.
    void SetSaveDataTitleId(const std::string& title_id);
    std::string GetSaveDataDir();

    // Direct-memory phys pool access for the demand-commit fault handler.
    // IsPhysPoolAddress reports whether `addr` lies inside the 2 GB direct
    // memory reservation; CommitPhysPool commits the 64 KiB block covering
    // `addr` (read/write) and returns true on success.
    bool IsPhysPoolAddress(guest_addr_t addr);
    bool CommitPhysPool(guest_addr_t addr);

    // AGC → VideoOut flip forwarding (libvideoout.cpp): runs the SubmitFlip
    // path (counters, flip events, GPU present) for an RFlip packet found in
    // an AGC DCB.  Returns the SubmitFlip error code (0 on success).
    u64 VideoOutSubmitFlipFromAgc(u32 handle, s32 buffer_index, u32 flip_mode, s64 flip_arg);

    // Display-buffer lookup for the AGC deferred-composite path (M3.3):
    // resolves the guest address + dimensions of a registered display buffer
    // so a targetless draw can be retargeted at it when its RFlip arrives.
    bool VideoOutGetDisplayBufferInfo(u32 handle, s32 buffer_index,
                                      guest_addr_t* addr, u32* width, u32* height);

    // Push the configured audio output settings into libSceAudioOut
    // (libaudioout.cpp).  Called from main() after the config service loads;
    // backend: 0 = Off (silent, real-time paced), non-zero = waveOut.
    void SetAudioOutConfig(int backend, float volume);

    // AGC submitted-DCB walker introspection (libagc.cpp; tests + M1-M3).
    // which: 0 = total draws, 1 = total dispatches, 2 = total flips (graphics queue).
    u64 AgcGetSubmittedStats(u32 which);
    // Reads the graphics-queue register shadow (space: 0 = cx, 1 = sh, 2 = uc).
    bool AgcGetShadowRegister(u32 space, u32 reg, u32* value_out);

    // Cooperative guest shutdown ------------------------------------------------
    // The GLFW window lives on the process main thread (which runs the event
    // loop); guest code runs on a dedicated worker thread.  Window close sets
    // the stop flag via RequestStop(); the HLE dispatch path observes it and
    // terminates the guest via ExitGuestProcess(), which longjmps back into
    // Kernel::Execute (SEH unwinding cannot cross guest/asm frames, so a
    // setjmp/longjmp pair is used instead — same thread, host stack).
    // Called by Kernel::Execute so the exit path knows which thread carries
    // the armed setjmp buffer.
    void SetMainGuestThreadId(unsigned long thread_id);
    void RequestStop();
    bool StopRequested();
    // setjmp buffer armed by Kernel::Execute while the guest runs.
    jmp_buf& GuestExitEnv();
    void ArmGuestExitEnv(bool armed);
    u32 GuestExitCode();
    // Terminate the guest with `exit_code`: longjmps back into
    // Kernel::Execute on the main guest thread; from any other thread there
    // is no armed buffer, so it falls back to std::exit() (legacy behaviour).
    [[noreturn]] void ExitGuestProcess(u32 exit_code);

    // Dynamic dispatcher callback (called by assembly bridge).
    // guest_rsp is the guest stack pointer at thunk entry (points at the guest
    // return address; the first SysV stack argument is at guest_rsp + 8).
    extern "C" u64 HleDispatch(u64 symbol_id, u64 rdi, u64 rsi, u64 rdx, u64 rcx, u64 r8, u64 r9, u64 guest_rip, u64 guest_rsp);
}
// namespace HLE

// Assembly-language trampoline: call a guest function from within a Windows-ABI C++ function.
// Translates Windows ABI → System V ABI and stores/restores the per-thread host stack pointer
// in a __declspec(thread) variable (thread-safe, each host thread has its own slot).
// rcx = guest_func_va, rdx = rdi_arg, r8 = rsi_arg, r9 = rdx_arg
// Returns: rax = guest return value
extern "C" u64 InvokeGuestFunction(u64 guest_func_va, u64 rdi_arg, u64 rsi_arg, u64 rdx_arg);

// Six-argument variant used by the HLE C++ exception unwinder (liblibc.cpp)
// to call guest personality routines (5 args) and exception destructors.
// rcx = guest_func_va, rdx = pointer to an array of 6 u64 SysV arguments
// (rdi, rsi, rdx, rcx, r8, r9).  Returns: rax = guest return value.
extern "C" u64 InvokeGuestFunction6(u64 guest_func_va, const u64* args6);

// Per-thread host stack pointer helpers called by dispatcher.asm.
// Each guest/host thread gets its own private copy via __declspec(thread).
extern "C" uintptr_t GetHostStackPointer();
extern "C" void SetHostStackPointer(uintptr_t rsp);

