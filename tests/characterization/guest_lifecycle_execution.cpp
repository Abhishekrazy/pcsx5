// Real extracted entry/exit state with original synthetic guest snippets.
// Each potentially fatal path runs in a child; no fake exit/recovery is used.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "kernel/guest_execution.h"
#include "hle/guest_lifecycle.h"
#ifdef PCSX5_WITH_REAL_KERNEL_VEH
#include "kernel/exception_entry.h"
#include "kernel/syscalls.h"
#include "kernel/kernel.h"
#include "hle/hle.h"
#include "memory/memory.h"
#endif
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

extern "C" {
u64 lifecycle_gpr_before[8]{}, lifecycle_gpr_after[8]{};
u64 lifecycle_xmm_before[20]{}, lifecycle_xmm_after[20]{};
u64 lifecycle_rsp_before{}, lifecycle_rsp_after{};
u64 lifecycle_fault_target{}, lifecycle_syscall_result{};
void RunCapturedProbe(u64 entry, u64 stack);
void LifecycleExitGuest();
void LifecycleFaultGuest();
void LifecycleFaultStore();
void LifecycleProtectedFaultGuest();
void LifecycleProtectedFaultStore();
void LifecycleSyscallGuest();
void LifecycleSysExitGuest();
}
namespace {
struct Evidence {
    DWORD started{};
    DWORD exit_callback{};
    DWORD returned{};
    DWORD captured_ok{};
    DWORD exit_code{};
    DWORD tib_restored{};
    DWORD host_gpr_restored{};
    DWORD host_xmm_restored{};
    DWORD host_rsp_restored{};
    DWORD stop_requested{};
    DWORD observed_guest_av{};
    DWORD syscall_calls{};
    DWORD syscall_arguments{};
    u64 syscall_result{};
    DWORD exit_hook_calls{};
    DWORD diagnostic_reads{};
    DWORD timeline_reads{};
    DWORD kernel_fault_returned{};
    DWORD guest_crash_recorded{};
};
Evidence* evidence{};
u64 allocated_fault_rip{};
int failures{};
void Check(bool ok, const char* text) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", text); }
}
}
LONG CALLBACK ObserveOnly(EXCEPTION_POINTERS* exception) {
    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        (exception->ContextRecord->Rip == reinterpret_cast<u64>(&LifecycleFaultStore) ||
         exception->ContextRecord->Rip == reinterpret_cast<u64>(&LifecycleProtectedFaultStore) ||
         exception->ContextRecord->Rip == allocated_fault_rip))
        ++evidence->observed_guest_av;
    return EXCEPTION_CONTINUE_SEARCH; // Observation only: never recover or alter a context.
}
#pragma warning(push)
#pragma warning(disable: 4611)
void HostExitControl(bool syscall_exit = false) {
    if (setjmp(HLE::GuestExitEnv()) == 0) {
        HLE::ArmGuestExitEnv(true);
#ifdef PCSX5_WITH_REAL_KERNEL_VEH
        if (syscall_exit) {
            Kernel::SetInProcMode(true);
            Kernel::SysExit(42, nullptr);
            std::abort();
        }
#else
        (void)syscall_exit;
#endif
        HLE::ExitGuestProcess(42);
    }
    evidence->returned = 1;
    evidence->exit_code = HLE::GuestExitCode();
    HLE::ArmGuestExitEnv(false);
}
#pragma warning(pop)
DWORD WINAPI WorkerExit(void*) {
    ++evidence->exit_callback;
    HLE::ExitGuestProcess(42);
}
#pragma warning(push)
#pragma warning(disable: 4611)
int ArmedWorkerExitControl() {
    // Keep a live main-thread jump target while the worker exits. This tests
    // the thread-ID guard independently from the unarmed fallback case.
    if (setjmp(HLE::GuestExitEnv()) != 0) return 207;
    HLE::ArmGuestExitEnv(true);
    HANDLE worker = CreateThread(nullptr, 0, WorkerExit, nullptr, 0, nullptr);
    if (!worker) return 208;
    WaitForSingleObject(worker, 5000);
    CloseHandle(worker);
    return 202; // Correct legacy fallback terminates the process before here.
}
#pragma warning(pop)
extern "C" void CallCaptured(u64 entry, u64 stack) {
    u32 code = 0xdddd;
    evidence->captured_ok = Kernel::StartGuestCaptured(entry, stack, &code);
    evidence->exit_code = code;
    ++evidence->returned;
}
extern "C" void FixtureExit() {
    ++evidence->exit_callback;
    evidence->syscall_result = lifecycle_syscall_result;
    HLE::ExitGuestProcess(42);
}
// Required only by unused HleCommonDispatcher in the same assembly object.
// Never substitutes for an exit, trap or recovery path exercised here.
extern "C" u64 HleDispatch(u64, u64, u64, u64, u64, u64, u64, u64, u64) {
    std::abort();
}
#ifdef PCSX5_WITH_REAL_KERNEL_VEH
std::vector<HLE::TraceEntry> EmptyHistory(std::size_t) { ++evidence->diagnostic_reads; return {}; }
std::vector<std::string> EmptyTimeline() { ++evidence->timeline_reads; return {}; }
LONG CALLBACK ObserveAfterKernel(EXCEPTION_POINTERS* exception) {
    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        exception->ContextRecord->Rip == allocated_fault_rip) {
        ++evidence->kernel_fault_returned;
        u32 code{};
        guest_addr_t rip{};
        evidence->guest_crash_recorded = HLE::GetLastGuestCrashInfo(&code, &rip, nullptr, 0) &&
            code == EXCEPTION_ACCESS_VIOLATION && rip == allocated_fault_rip;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
void LifecycleExitHook() {
    ++evidence->exit_hook_calls;
    Kernel::RunGuestExitHook(); // Same-thread reentry must not call us twice.
}
s64 LifecycleSysExit(CONTEXT* context) {
    ++evidence->syscall_calls;
    evidence->syscall_arguments = context->Rdi == 42;
    return Kernel::SysExit(static_cast<s32>(context->Rdi), context);
}
s64 LifecycleSyscall(CONTEXT* context) {
    ++evidence->syscall_calls;
    evidence->syscall_arguments = context->Rdi == 0x11 && context->Rsi == 0x22 &&
        context->Rdx == 0x33 && context->R10 == 0x44 && context->R8 == 0x55 && context->R9 == 0x66;
    return 0x1234;
}
#endif

int Child(const std::string& route, HANDLE mapping) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    evidence = static_cast<Evidence*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Evidence)));
    if (!evidence) return 200;
    evidence->started = 1;
    HLE::ResetGuestLifecycleState();
    HLE::SetMainGuestThreadId(GetCurrentThreadId());
    if (route == "host_control") { HostExitControl(); return 0; }
    if (route == "sys_exit_host_control") { HostExitControl(true); return 0; }
    if (route == "unarmed") { FixtureExit(); return 201; }
    if (route == "worker") return ArmedWorkerExitControl();
#ifdef PCSX5_WITH_REAL_KERNEL_VEH
    if (_putenv_s("PCSX5_BREAK_SYSCALL", "") != 0 ||
        !SetEnvironmentVariableA("PCSX5_WATCH_ADDR", nullptr) ||
        !SetEnvironmentVariableA("PCSX5_BP_ADDR", nullptr) ||
        !SetEnvironmentVariableA("PCSX5_CRASH_DUMP_SIZE", "4096")) return 212;
    if (!Memory::Initialize()) return 209;
    Kernel::Legacy::SetExceptionTraceReader(EmptyHistory);
    Kernel::Legacy::SetExceptionTimelineReader(EmptyTimeline);
    if (!AddVectoredExceptionHandler(1, Kernel::Legacy::ExceptionEntry())) return 210;
    Kernel::RegisterSyscallHandler(501, LifecycleSyscall);
    Kernel::RegisterSyscallHandler(1, LifecycleSysExit);
    Kernel::SetGuestExitHook(LifecycleExitHook);
    Kernel::SetInProcMode(route == "sys_exit_inproc");
    if (!AddVectoredExceptionHandler(0, ObserveAfterKernel)) return 213;
#endif
    void* allocation = VirtualAlloc(nullptr, 1024*1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!allocation) return 203;
    const auto stack = reinterpret_cast<u64>(allocation) + 1024*1024;
    auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    const auto base = tib->StackBase;
    const auto limit = tib->StackLimit;
    PVOID observer = AddVectoredExceptionHandler(1, ObserveOnly);
    if (!observer) return 206;
    for (std::size_t i=0; i!=20; ++i) lifecycle_xmm_before[i] = 0xaabbccdd00000000ULL + i;
    const bool fault = route == "fault" || route == "fault_direct";
    auto entry = reinterpret_cast<u64>(fault ? &LifecycleFaultGuest : &LifecycleExitGuest);
    if (route == "fault_veh") {
        lifecycle_fault_target = reinterpret_cast<u64>(VirtualAlloc(nullptr, 65536,
            MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
        if (!lifecycle_fault_target) return 211;
        entry = reinterpret_cast<u64>(&LifecycleProtectedFaultGuest);
    }
    if (route == "syscall_exit") entry = reinterpret_cast<u64>(&LifecycleSyscallGuest);
    if (route == "sys_exit_inproc" || route == "sys_exit_standalone")
        entry = reinterpret_cast<u64>(&LifecycleSysExitGuest);
#ifdef PCSX5_WITH_REAL_KERNEL_VEH
    if (route == "fault_guest_veh") {
        void* inaccessible = VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
        void* code = VirtualAlloc(reinterpret_cast<void*>(0x880000000ULL), 65536,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!inaccessible || !code) return 214;
        // Original synthetic bytes: mov rax, owned-inaccessible-page; mov rax,[rax]; int3.
        std::array<u8, 14> bytes{0x48, 0xb8};
        const auto target = reinterpret_cast<u64>(inaccessible);
        std::memcpy(bytes.data() + 2, &target, sizeof(target));
        bytes[10] = 0x48; bytes[11] = 0x8b; bytes[12] = 0x00; bytes[13] = 0xcc;
        std::memcpy(code, bytes.data(), bytes.size());
        DWORD previous{};
        if (!VirtualProtect(code, 65536, PAGE_EXECUTE_READ, &previous) ||
            !FlushInstructionCache(GetCurrentProcess(), code, bytes.size())) return 215;
        entry = reinterpret_cast<u64>(code);
        allocated_fault_rip = entry + 10;
        if (Memory::AdoptRange(entry, 65536, Memory::PROT_READ | Memory::PROT_EXEC,
                true, Memory::Owner::Guest, "synthetic-lifecycle-fault") != Memory::Status::Ok)
            return 216;
    }
#endif
    if (route == "exit_direct" || route == "fault_direct") CallCaptured(entry, stack);
    else RunCapturedProbe(entry, stack);
    evidence->tib_restored = tib->StackBase == base && tib->StackLimit == limit;
    evidence->host_gpr_restored = std::memcmp(lifecycle_gpr_before, lifecycle_gpr_after, sizeof(lifecycle_gpr_before)) == 0;
    evidence->host_xmm_restored = std::memcmp(lifecycle_xmm_before, lifecycle_xmm_after, sizeof(lifecycle_xmm_before)) == 0;
    evidence->host_rsp_restored = lifecycle_rsp_before == lifecycle_rsp_after;
    evidence->stop_requested = HLE::StopRequested();
    HLE::ResetGuestLifecycleState();
    RemoveVectoredExceptionHandler(observer);
    if (!VirtualFree(allocation, 0, MEM_RELEASE)) return 204;
    UnmapViewOfFile(evidence);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "--child") == 0)
        return Child(argv[2], reinterpret_cast<HANDLE>(std::strtoull(argv[3], nullptr, 10)));
    char executable[MAX_PATH]{};
    if (!GetModuleFileNameA(nullptr, executable, MAX_PATH)) return 1;
#ifdef PCSX5_WITH_REAL_KERNEL_VEH
    std::string trace = "{\"schema_version\":1,\"fixture\":\"guest_lifecycle_kernel\",\"events\":[";
#else
    std::string trace = "{\"schema_version\":1,\"fixture\":\"guest_lifecycle_execution\",\"events\":[";
#endif
    unsigned sequence{};
    auto event = [&](const char* subject, const char* value) {
        if (sequence) trace += ',';
        trace += "{\"sequence\":" + std::to_string(sequence++) +
            ",\"kind\":\"check\",\"subject\":\"" + subject + "\",\"value\":\"" + value + "\"}";
    };
    std::vector<const char*> routes{"host_control", "exit", "exit_direct", "fault", "fault_direct", "unarmed", "worker"};
#ifdef PCSX5_WITH_REAL_KERNEL_VEH
    // Avoid the unrelated legacy null-pointer recovery heuristic. These routes
    // combine the real VEH, syscall dispatcher, StartGuest and HLE exit state.
    routes = {"sys_exit_host_control", "syscall_exit", "fault_veh", "fault_guest_veh",
        "sys_exit_inproc", "sys_exit_standalone"};
#endif
    for (const char* route : routes) {
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(Evidence), nullptr);
        if (!mapping) return 1;
        auto* record = static_cast<Evidence*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Evidence)));
        if (!record) { CloseHandle(mapping); return 1; }
        *record = {};
        std::string command = std::string("\"") + executable + "\" --child " + route + " " +
            std::to_string(reinterpret_cast<std::uintptr_t>(mapping));
        STARTUPINFOA startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const bool created = CreateProcessA(executable, command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != 0;
        Check(created, "create isolated lifecycle child");
        if (created) {
            const DWORD wait = WaitForSingleObject(process.hProcess, 5000);
            Check(wait == WAIT_OBJECT_0, "child completes within five seconds");
            if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 205); WaitForSingleObject(process.hProcess, 1000); }
            DWORD code{};
            Check(GetExitCodeProcess(process.hProcess, &code) != 0, "read child outcome");
            std::printf("route=%s process=0x%08lx started=%lu callback=%lu returned=%lu ok=%lu guest_code=%lu tib=%lu gpr=%lu xmm=%lu rsp=%lu stop=%lu guest_av=%lu syscalls=%lu syscall_args=%lu syscall_result=%llu\n",
                route, code, record->started, record->exit_callback, record->returned,
                record->captured_ok, record->exit_code, record->tib_restored,
                record->host_gpr_restored, record->host_xmm_restored, record->host_rsp_restored,
                record->stop_requested, record->observed_guest_av, record->syscall_calls,
                record->syscall_arguments, static_cast<unsigned long long>(record->syscall_result));
            std::printf("  hooks=%lu history=%lu timeline=%lu kernel_fault_returned=%lu guest_crash_recorded=%lu\n",
                record->exit_hook_calls, record->diagnostic_reads, record->timeline_reads,
                record->kernel_fault_returned, record->guest_crash_recorded);
            Check(record->started == 1, "child reached fixture before outcome");
            char normalized_code[19]{};
            std::snprintf(normalized_code, sizeof(normalized_code), "0x%016llx", static_cast<unsigned long long>(code));
            if (std::strcmp(route, "sys_exit_inproc") == 0) {
                // Repetition observed both AV and BAD_STACK for this broken
                // unwind path. Keep exact status in stdout; the deterministic
                // summary records only the checked failure to return.
                event("sys_exit_inproc_raw_status", "unknown");
                event("sys_exit_inproc_recovery", record->returned ? "pass" : "fail");
            } else event(route, normalized_code);
            if (std::strcmp(route, "syscall_exit") == 0) {
                std::snprintf(normalized_code, sizeof(normalized_code), "0x%016llx",
                    static_cast<unsigned long long>(record->syscall_result));
                event("observed_syscall_rax", normalized_code);
                event("syscall_arguments", record->syscall_arguments ? "pass" : "fail");
            }
            if (std::strcmp(route, "fault_guest_veh") == 0)
                event("kernel_recorded_guest_fault", record->guest_crash_recorded ? "pass" : "fail");
            if (std::strcmp(route, "sys_exit_standalone") == 0)
                event("sys_exit_cleanup_once", record->exit_hook_calls == 1 ? "pass" : "fail");
            // Termination prevents a post-return measurement; do not encode it as a false register comparison.
            if (std::strcmp(route, "exit") == 0 || std::strcmp(route, "fault") == 0 ||
                std::strcmp(route, "syscall_exit") == 0 || std::strcmp(route, "fault_veh") == 0 ||
                std::strcmp(route, "fault_guest_veh") == 0 || std::strcmp(route, "sys_exit_inproc") == 0)
                event("host_restoration", !record->returned ? "unknown" :
                    (record->tib_restored && record->host_gpr_restored &&
                     record->host_xmm_restored && record->host_rsp_restored ? "pass" : "fail"));
            if (std::strcmp(route, "sys_exit_inproc") == 0 || std::strcmp(route, "sys_exit_standalone") == 0) {
                const bool inproc = std::strcmp(route, "sys_exit_inproc") == 0;
                Check(record->syscall_calls == 1 && record->syscall_arguments == 1 &&
                    record->exit_callback == 0 && record->returned == 0,
                    "real SysExit is reached through a trap with requested status");
                // Repeated runs observed AV and BAD_STACK after real SysExit.
                // Accept only those recorded fatal outcomes, never arbitrary
                // termination or timeout; successful recovery needs a new gate.
                const bool expected_status = inproc ?
                    (code == EXCEPTION_ACCESS_VIOLATION || code == 0xc0000028UL) : code == 42;
                Check(expected_status &&
                    record->exit_hook_calls == (inproc ? 0UL : 1UL),
                    "SysExit: in-proc retained stack failure; standalone exact-once hook and requested process exit");
            } else if (std::strcmp(route, "unarmed") == 0 || std::strcmp(route, "worker") == 0) {
                Check(code == 42 && record->exit_callback == 1 && record->returned == 0,
                    "real HLE unarmed/off-main exit terminates child with requested code");
            } else if (std::strcmp(route, "host_control") == 0 ||
                       std::strcmp(route, "sys_exit_host_control") == 0) {
                Check(code == 0 && record->returned == 1 && record->exit_code == 42,
                    "real HLE longjmp works on the host-stack control");
            } else if (std::strcmp(route, "exit") == 0 || std::strcmp(route, "exit_direct") == 0 ||
                       std::strcmp(route, "syscall_exit") == 0) {
                // RED investigation: intended captured return failed identically with
                // the sentinel wrapper and a direct C++ call. Host-stack control passes.
                // Pin the observed legacy failure; never report restoration as verified.
                constexpr DWORD legacy_bad_stack = 0xc0000028;
                Check(code == legacy_bad_stack && record->exit_callback == 1 && record->returned == 0,
                    "KNOWN DEFECT: real switched-stack exit terminates before host restoration");
                if (std::strcmp(route, "syscall_exit") == 0)
                    Check(record->syscall_calls == 1 && record->syscall_arguments == 1 && record->syscall_result == 0x1234,
                        "real syscall trap resumes on dedicated guest stack before real exit failure");
            } else {
                Check(code == EXCEPTION_ACCESS_VIOLATION && record->returned == 0 &&
                    record->exit_callback == 0 && record->observed_guest_av == 1,
                    "KNOWN DEFECT: exact synthetic guest AV terminates instead of reaching host SEH return");
                if (std::strcmp(route, "fault_guest_veh") == 0)
                    Check(record->kernel_fault_returned == 1 && record->guest_crash_recorded == 1 &&
                        record->diagnostic_reads >= 1,
                        "tracked guest fault reaches real kernel classification before failed host recovery");
            }
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
        }
        UnmapViewOfFile(record); CloseHandle(mapping);
    }
    if (!failures && argc == 2) {
        std::ofstream output(argv[1], std::ios::binary);
        output << trace << "]}\n";
        output.close();
        if (!output) return 1;
    }
    return failures ? 1 : 0;
}
