// Behavior-preserving extraction from kernel.cpp for Phase 0 characterization.
// Known stack/exit-state defects are intentionally retained.
#include "guest_execution.h"
#include "../hle/guest_lifecycle.h"
#include "../common/log.h"
#include <windows.h>
#include <cstdio>

namespace Kernel {
    extern "C" void StartGuest(u64 entry_point, u64 stack_pointer);

    static bool TryStartGuest(guest_addr_t entry_point, guest_addr_t sp) {
#ifdef _WIN32
        PNT_TIB tib = (PNT_TIB)NtCurrentTeb();
        PVOID host_stack_base = tib->StackBase;
        PVOID host_stack_limit = tib->StackLimit;
        // Spoof bounds around the dedicated guest stack
        tib->StackBase = (PVOID)(sp + 0x800000);
        tib->StackLimit = (PVOID)(sp - 0x800000);
#endif
        bool ok = true;
        __try {
            StartGuest(entry_point, sp);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG_ERROR(Kernel, "Unhandled hardware exception occurred inside guest execution!");
            ok = false;
        }
#ifdef _WIN32
        tib->StackBase = host_stack_base;
        tib->StackLimit = host_stack_limit;
#endif
        return ok;
    }

    // Cooperative guest exit: HleDispatch observes the window-close stop flag
    // (and guest exit()/libc exit paths call HLE::ExitGuestProcess directly),
    // which longjmps back to the setjmp below.  SEH unwinding cannot cross
    // guest/asm frames, so a setjmp/longjmp pair on the same (host) stack is
    // used instead.  C4611 is suppressed locally: the longjmp target frame
    // holds no C++ objects, and frames abandoned by the jump (guest/asm and
    // the current HleDispatch) intentionally skip destruction.
#pragma warning(push)
#pragma warning(disable: 4611)
    bool StartGuestCaptured(guest_addr_t entry_point, guest_addr_t sp, u32* out_exit_code) {
#ifdef _WIN32
        PNT_TIB tib = (PNT_TIB)NtCurrentTeb();
        PVOID host_stack_base = tib->StackBase;
        PVOID host_stack_limit = tib->StackLimit;
#endif
        if (setjmp(HLE::GuestExitEnv()) == 0) {
            HLE::ArmGuestExitEnv(true);
            bool ok = TryStartGuest(entry_point, sp);
            HLE::ArmGuestExitEnv(false);
            return ok;
        }
#ifdef _WIN32
        // Restore TEB if we longjmp'd out of TryStartGuest!
        tib->StackBase = host_stack_base;
        tib->StackLimit = host_stack_limit;
#endif
        *out_exit_code = HLE::GuestExitCode();
        LOG_INFO(Kernel, "Guest requested process termination (exit code %u).", *out_exit_code);
        printf("StartGuestCaptured Returning True\n");
        return true;
    }
#pragma warning(pop)
}
