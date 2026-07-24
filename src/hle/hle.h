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
        // SysV float/vector argument registers (XMM0-XMM7) captured by the dispatcher.
        u64 xmm_args[8] = {};
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
    u64 GetIncomingXmm0();
    const u64* GetIncomingXmmBlock();

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

    // Register libSceJson / sce::Json HLE handlers
    void RegisterLibJson();

    // Get the incoming XMM0 argument (float/double) from the current HLE dispatch.
    // Set by the dispatcher asm before each call via SetIncomingXmm0.
    u64 GetIncomingXmm0();

    // Resolve symbol by exact module+name key; returns thunk address or 0
    guest_addr_t Resolve(const std::string& module_name, const std::string& name);

    // Resolve by NID name alone, searching all registered modules.
    // Falls back to auto-stub creation if not found anywhere (unless strict mode).
    guest_addr_t ResolveAny(const std::string& name);

    // Store the guest VA of the game's main() function (called from ELF loader)
    void SetGuestMainAddress(guest_addr_t addr);
    guest_addr_t GetGuestMainAddress();

    // Register a module's .eh_frame_hdr (guest VA + size) with the HLE C++
    // exception unwinder (liblibc.cpp).  Called from main() for the main
    // module and from the kernel loader for every auto-loaded PRX; each call
    // appends one table (duplicates ignored) so unwinding can cross module
    // boundaries.  addr==0 is a no-op; with no registered tables __cxa_throw
    // logs and survives instead of unwinding.
    void SetGuestEhFrameHdr(guest_addr_t addr, u64 size);

    // True when `addr` falls inside the allocated HLE thunk page (i.e. it is
    // one of our import stubs, not real guest code).  Used by the C++
    // exception unwinder to detect CIE personality pointers that the guest's
    // dynamic linker resolved to an imported HLE __gxx_personality_v0 stub;
    // those are evaluated natively over the frame LSDA instead of being
    // called as guest code.
    bool IsHleThunkAddress(u64 addr);

    // Store the VA of the DT_INIT function (global ctor runner) for re-invocation if needed
    void SetDtInitAddress(guest_addr_t addr);
    guest_addr_t GetDtInitAddress();

    // Save-data host backing directory (libsavedata.cpp).  The title id is
    // supplied by main() from --title-id; GetSaveDataDir() creates and returns
    // <cwd>/pcsx5_savedata/<title-id>/.
    void SetSaveDataTitleId(const std::string& title_id);
    std::string GetSaveDataDir();

    // Effective savedata dir for guest file I/O and dir enumeration:
    // GetSaveDataDir() plus a per-user level when multiple user profiles are
    // configured; the flat per-title dir otherwise (migration-safe).  Trophies
    // deliberately stay on the flat dir.
    std::string GetEffectiveSaveDataDir();

    // Trophy unlock store introspection (libnp.cpp).  NpTrophyUnlockedCount
    // returns the number of persisted unlocked trophies (loaded lazily from
    // <savedata-dir>/trophies.json); NpTrophyIsUnlocked reports whether the
    // given trophy id has been recorded.
    size_t NpTrophyUnlockedCount();
    bool NpTrophyIsUnlocked(s32 trophy_id);

    // Per-title keystone blob (libkeystone.cpp).  main() loads and validates
    // <app0>/.keystone at boot and stashes the raw bytes here; an empty blob
    // means the title shipped no keystone (not an error).
    void SetKeystoneBlob(std::vector<u8> blob);
    const std::vector<u8>& GetKeystoneBlob();

    // Guest errno cell backing libc __error() (liblibc.cpp).  HLE handlers
    // implementing the libc/POSIX ABI (-1 with errno set on failure, e.g. the
    // POSIX file exports in libkernel.cpp) record the errno through
    // SetGuestErrno so guest code reading *__error() observes it.
    s32* GuestErrnoPtr();
    void SetGuestErrno(s32 value);

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
    void VideoOutSetVrrMode(bool active);

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

    // Guest crash tracking -------------------------------------------------------
    // Called by the VEH handler when a guest crash is detected (RIP in guest
    // code range).  Instead of killing the process, the VEH sets this flag and
    // returns so the SEH handler in TryStartGuest can clean up.
    void SetGuestCrashed(u32 exception_code, guest_addr_t rip);

    // True when the guest has crashed since the last ResetRunStatistics().
    bool IsGuestCrashed();

    // Retrieves the last guest crash info.  Returns true if a crash is recorded.
    // `out_buf` is filled with a human-readable description (NUL-terminated).
    // Pass buf_size=0 to ignore the string; returns true even then if crash data
    // exists.
    bool GetLastGuestCrashInfo(u32* out_exception_code, guest_addr_t* out_rip,
                               char* out_buf, int buf_size);

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

