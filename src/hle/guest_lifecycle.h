#pragma once

#include "../common/types.h"
#include <csetjmp>

// Legacy Windows direct-execution state, separated for Phase 0 characterization.
// This is not a host-independent runtime contract.
namespace HLE {
    void SetMainGuestThreadId(unsigned long thread_id);
    void RequestStop();
    bool StopRequested();
    jmp_buf& GuestExitEnv();
    void ArmGuestExitEnv(bool armed);
    u32 GuestExitCode();
    [[noreturn]] void ExitGuestProcess(u32 exit_code);

    void SetGuestCrashed(u32 exception_code, guest_addr_t rip);
    bool IsGuestCrashed();
    bool GetLastGuestCrashInfo(u32* out_exception_code, guest_addr_t* out_rip,
                               char* out_buf, int buf_size);

    // Exact reset scopes formerly embedded in ResetRunStatistics and Shutdown.
    // Call lifecycle reset only once guest execution has stopped.
    void ResetGuestCrashState();
    void ResetGuestLifecycleState();

    u64 GetIncomingXmm0();
    const u64* GetIncomingXmmBlock();
}

extern "C" uintptr_t GetHostStackPointer();
extern "C" void SetHostStackPointer(uintptr_t rsp);
extern "C" void SetIncomingXmm0(u64 val);
extern "C" void SetIncomingXmmBlock(const u64* ptr);
