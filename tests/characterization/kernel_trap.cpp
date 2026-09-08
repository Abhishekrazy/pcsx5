// Original synthetic fixtures for the real legacy kernel VEH. Each CTest case
// is a fresh process. These run machine code on the host stack, not StartGuest.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "kernel/exception_entry.h"
#include "kernel/kernel.h"
#include "kernel/syscalls.h"
#include "hle/hle.h"
#include "memory/memory.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <string_view>
#include <vector>

namespace {
constexpr u32 syscall_id = 501;
constexpr s64 syscall_result = -123;
constexpr std::array<u64, 6> args{0x11223344, 0x22334455, 0x33445566,
                                0x44556677, 0x55667788, 0x66778899};
int handler_calls = 0;
int diagnostic_reads = 0;
int timeline_reads = 0;
bool args_match = false;
CONTEXT handler_context{};
DWORD caught_exception = 0;
int exit_hook_calls = 0;
u64 observed_syscall_result = 0;

void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL kernel trap: %s\n", message);
        std::exit(91);
    }
}

void ExitHook() {
    ++exit_hook_calls;
    std::printf("exit_hook=%d\n", exit_hook_calls);
    std::fflush(stdout);
    Kernel::RunGuestExitHook(); // Real reentrant call must not invoke us again.
}

void ExitHookOnce() {
    Kernel::SetGuestExitHook(&ExitHook);
    Kernel::RunGuestExitHook();
    Kernel::RunGuestExitHook();
    Check(exit_hook_calls == 1, "real cleanup hook executes once across reentry/repetition");
}

std::vector<HLE::TraceEntry> EmptyDiagnosticHistory(std::size_t count) {
    Check(count == 16, "original diagnostic history count");
    ++diagnostic_reads;
    return {}; // Diagnostic data only; no emulation or lifecycle replacement.
}

std::vector<std::string> EmptyBootTimeline() {
    ++timeline_reads;
    return {}; // Explicit empty diagnostic input, not a graphics implementation.
}

s64 SyntheticSyscall(CONTEXT* context) {
    ++handler_calls;
    handler_context = *context;
    args_match = context->Rdi == args[0] && context->Rsi == args[1] &&
        context->Rdx == args[2] && context->R10 == args[3] &&
        context->R8 == args[4] && context->R9 == args[5];
    return syscall_result;
}

int CaptureException(DWORD code) {
    caught_exception = code;
    return EXCEPTION_EXECUTE_HANDLER;
}

// No C++ objects in this SEH frame. The OS delivers the trap through the real
// registered VEH before reaching this test-owned last-chance catch.
void InvokeCaught(void* code) {
    __try {
        reinterpret_cast<void (*)()>(code)();
    } __except (CaptureException(GetExceptionCode())) {
    }
}

void Append64(std::vector<u8>& bytes, u64 value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}

void Mov64(std::vector<u8>& bytes, u8 rex, u8 opcode, u64 value) {
    bytes.push_back(rex);
    bytes.push_back(opcode);
    Append64(bytes, value);
}

void* MakeCode(const std::vector<u8>& bytes, bool guest, bool tracked) {
    void* requested = guest ? reinterpret_cast<void*>(0x880000000ULL) : nullptr;
    void* code = VirtualAlloc(requested, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    Check(code != nullptr, "owned code allocation");
    const u64 address = reinterpret_cast<u64>(code);
    Check(guest == (address >= Memory::kGuestVaBase && address < Memory::kGuestVaEnd),
          "code belongs to requested legacy numeric address class");
    std::memcpy(code, bytes.data(), bytes.size());
    DWORD previous = 0;
    Check(VirtualProtect(code, 65536, PAGE_EXECUTE_READ, &previous) != FALSE,
          "code W-to-X transition");
    Check(FlushInstructionCache(GetCurrentProcess(), code, bytes.size()) != FALSE,
          "instruction cache flush");
    if (tracked) {
        Check(Memory::AdoptRange(address, 65536, Memory::PROT_READ | Memory::PROT_EXEC,
            true, Memory::Owner::Guest, "synthetic-kernel-trap") == Memory::Status::Ok,
            "code tracking");
    }
    return code;
}

void ReleaseCode(void* code, bool tracked) {
    if (tracked) {
        Check(Memory::ForgetResource(reinterpret_cast<u64>(code)) == Memory::Status::Ok,
              "drop owned code tracking");
    }
    Check(VirtualFree(code, 0, MEM_RELEASE) != FALSE, "release owned code");
}

void VerifyCrash(bool crashed, DWORD code, u64 rip, int expected_history_reads = 0) {
    Check(HLE::IsGuestCrashed() == crashed, "real HLE guest crash classification");
    u32 recorded_code = 0;
    guest_addr_t recorded_rip = 0;
    Check(HLE::GetLastGuestCrashInfo(&recorded_code, &recorded_rip, nullptr, 0) == crashed,
          "real HLE crash record presence");
    if (crashed) {
        Check(recorded_code == code && recorded_rip == rip, "real crash code and RIP");
    }
    Check(diagnostic_reads == expected_history_reads, "expected real-path diagnostic history reads");
}

void ActualSyscall(bool guest) {
    std::array<u64, 9> output{};
    // Save the Windows nonvolatile registers changed by this authored SysV
    // fixture. Trap arguments and post-trap values remain real machine state.
    std::vector<u8> bytes{0x53, 0x57, 0x56}; // push rbx/rdi/rsi
    Mov64(bytes, 0x48, 0xBB, reinterpret_cast<u64>(output.data()));
    Mov64(bytes, 0x48, 0xBF, args[0]);
    Mov64(bytes, 0x48, 0xBE, args[1]);
    Mov64(bytes, 0x48, 0xBA, args[2]);
    Mov64(bytes, 0x49, 0xBA, args[3]);
    Mov64(bytes, 0x49, 0xB8, args[4]);
    Mov64(bytes, 0x49, 0xB9, args[5]);
    Mov64(bytes, 0x48, 0xB9, 0x778899AA);
    Mov64(bytes, 0x49, 0xBB, 0x8899AABB);
    Mov64(bytes, 0x48, 0xB8, syscall_id);
    const auto trap_offset = bytes.size();
    bytes.insert(bytes.end(), {0xCC, 0x90,
        0x48, 0x89, 0x03,       // [rbx] = rax
        0x48, 0x89, 0x7B, 0x08, // [rbx+8] = rdi
        0x48, 0x89, 0x73, 0x10,
        0x48, 0x89, 0x53, 0x18,
        0x4C, 0x89, 0x53, 0x20,
        0x4C, 0x89, 0x43, 0x28,
        0x4C, 0x89, 0x4B, 0x30,
        0x48, 0x89, 0x4B, 0x38,
        0x4C, 0x89, 0x5B, 0x40,
        0x5E, 0x5F, 0x5B, 0xC3});
    void* code = MakeCode(bytes, guest, guest);
    Kernel::RegisterSyscallHandler(syscall_id, &SyntheticSyscall);
    InvokeCaught(code);
    Check(caught_exception == 0, "CC90 resumes to synthetic caller");
    Check(handler_calls == 1 && args_match, "real trap-to-table six-register ABI");
    Check(handler_context.Rip == reinterpret_cast<u64>(code) + trap_offset,
          "Windows delivered original CC address to handler");
    Check(output[0] == static_cast<u64>(syscall_result), "signed return reaches guest RAX");
    observed_syscall_result = output[0];
    for (std::size_t i = 0; i < args.size(); ++i) {
        Check(output[i + 1] == args[i], "syscall argument register preserved on resume");
    }
    Check(output[7] == 0x778899AA && output[8] == 0x8899AABB,
          "legacy CC90 preserves RCX/R11 (not native SYSCALL semantics)");
    VerifyCrash(false, 0, 0);
    ReleaseCode(code, guest);
}

void DirectContext() {
    void* code = MakeCode({0xCC, 0x90, 0xC3}, false, false);
    CONTEXT context{};
    RtlCaptureContext(&context);
    context.Rip = reinterpret_cast<u64>(code);
    context.Rax = syscall_id;
    context.Rdi = args[0]; context.Rsi = args[1]; context.Rdx = args[2];
    context.R10 = args[3]; context.R8 = args[4]; context.R9 = args[5];
    CONTEXT expected = context;
    expected.Rax = static_cast<u64>(syscall_result);
    expected.Rip += 2;
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_BREAKPOINT;
    record.ExceptionAddress = code;
    EXCEPTION_POINTERS pointers{&record, &context};
    Kernel::RegisterSyscallHandler(syscall_id, &SyntheticSyscall);
    Check(Kernel::Legacy::ExceptionEntry()(&pointers) == EXCEPTION_CONTINUE_EXECUTION,
          "direct real handler result");
    Check(handler_calls == 1 && args_match, "direct real syscall table path");
    Check(std::memcmp(&context, &expected, sizeof(context)) == 0,
          "direct CONTEXT changes only RAX and RIP, including all modeled XMM/native bytes");
    observed_syscall_result = context.Rax;
    ReleaseCode(code, false);
}

void ActualRejected(bool guest, bool tracked, bool inproc, bool access_violation) {
    Kernel::SetInProcMode(inproc);
    void* inaccessible = nullptr;
    std::vector<u8> bytes;
    std::size_t fault_offset = 0;
    if (access_violation) {
        inaccessible = VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
        Check(inaccessible != nullptr, "owned inaccessible fault target");
        Mov64(bytes, 0x48, 0xB8, reinterpret_cast<u64>(inaccessible));
        fault_offset = bytes.size();
        bytes.insert(bytes.end(), {0x48, 0x8B, 0x00, 0xC3});
    } else {
        bytes = {0xCC, 0xC3}; // not the CC90 syscall signature
    }
    void* code = MakeCode(bytes, guest, tracked);
    Memory::MemoryInfo native_info{};
    Check(Memory::Query(reinterpret_cast<u64>(code), &native_info) == Memory::Status::Ok,
          "legacy Query accepts allocated native pages even without tracking");
    Check(Memory::QueryOwner(reinterpret_cast<u64>(code)) ==
          (tracked ? Memory::Owner::Guest : Memory::Owner::None),
          "fixture ownership remains distinct from native page query");
    InvokeCaught(code);
    const DWORD expected = access_violation ? EXCEPTION_ACCESS_VIOLATION : EXCEPTION_BREAKPOINT;
    Check(caught_exception == expected, "unclaimed real exception reaches outer host SEH");
    Check(handler_calls == 0, "ordinary fault does not dispatch syscall");
    // Defect characterization: in-proc uses Query rather than owned-region
    // membership, so untracked allocated code still passes its ownership gate.
    const bool crashed = guest;
    const int expected_reads = guest ? 1 : (access_violation ? 2 : 0);
    VerifyCrash(crashed, expected, reinterpret_cast<u64>(code) + fault_offset, expected_reads);
    Check(timeline_reads == (!guest && access_violation ? 1 : 0),
          "host AV reaches diagnostic timeline despite in-proc mode");
    ReleaseCode(code, tracked);
    if (inaccessible) Check(VirtualFree(inaccessible, 0, MEM_RELEASE) != FALSE,
                            "release inaccessible target");
}

void WriteSummary(const char* path, std::string_view route) {
    // Post-assertion semantic summary only, never instruction/state replay.
    // Values below are observed scalars, not ASLR addresses or OS identities.
    std::ofstream stream(path, std::ios::binary);
    stream << "{\"schema_version\":1,\"fixture\":\"kernel_trap_" << route
           << "\",\"events\":[\n";
    unsigned sequence = 0;
    auto scalar = [&](const char* subject, u64 value) {
        stream << "{\"sequence\":" << std::dec << sequence++
               << ",\"kind\":\"register\",\"subject\":\"" << subject
               << "\",\"value\":\"0x" << std::hex << std::setfill('0') << std::setw(16)
               << value << "\"},\n";
    };
    scalar("handler_calls", static_cast<u64>(handler_calls));
    if (handler_calls != 0) scalar("guest_rax", observed_syscall_result);
    scalar("caught_exception", caught_exception);
    scalar("guest_crash_flag", HLE::IsGuestCrashed() ? 1 : 0);
    scalar("diagnostic_history_reads", static_cast<u64>(diagnostic_reads));
    scalar("diagnostic_timeline_reads", static_cast<u64>(timeline_reads));
    scalar("exit_hook_calls", static_cast<u64>(exit_hook_calls));
    stream << "{\"sequence\":" << std::dec << sequence
           << ",\"kind\":\"exit\",\"subject\":\"fixture_assertions\",\"value\":\"pass\"}]}\n";
    stream.close();
    Check(static_cast<bool>(stream), "write normalized measured semantic summary");
}
}

int main(int argc, char** argv) {
    Check(argc == 2 || argc == 3, "named child-process case and optional summary path required");
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    // Clear external diagnostic run knobs. The real tracer has no registered
    // breakpoints in this process; its implementation, not comments, governs it.
    SetEnvironmentVariableA("PCSX5_WATCH_ADDR", nullptr);
    SetEnvironmentVariableA("PCSX5_BP_ADDR", nullptr);
    SetEnvironmentVariableA("PCSX5_GUEST_TRACE", nullptr);
    SetEnvironmentVariableA("PCSX5_BREAK_SYSCALL", nullptr);
    Check(_putenv_s("PCSX5_BREAK_SYSCALL", "") == 0, "clear CRT syscall debug environment");
    SetEnvironmentVariableA("PCSX5_CRASH_DUMP_SIZE", "4096");
    Check(Memory::Initialize(), "real memory initialization");
    Kernel::Legacy::SetExceptionTraceReader(&EmptyDiagnosticHistory);
    Kernel::Legacy::SetExceptionTimelineReader(&EmptyBootTimeline);
    PVOID registration = AddVectoredExceptionHandler(1, Kernel::Legacy::ExceptionEntry());
    Check(registration != nullptr, "real kernel VEH registration");
    const std::string_view route = argv[1];
    if (route == "syscall_guest") ActualSyscall(true);
    else if (route == "syscall_host") ActualSyscall(false);
    else if (route == "context") DirectContext();
    else if (route == "breakpoint_guest") ActualRejected(true, true, false, false);
    else if (route == "breakpoint_host") ActualRejected(false, false, false, false);
    else if (route == "breakpoint_inproc_untracked") ActualRejected(true, false, true, false);
    else if (route == "breakpoint_inproc_tracked") ActualRejected(true, true, true, false);
    else if (route == "fault_guest") ActualRejected(true, true, false, true);
    else if (route == "fault_host_inproc") ActualRejected(false, false, true, true);
    else if (route == "exit_hook") ExitHookOnce();
    else if (route == "sys_exit_child") {
        Kernel::SetGuestExitHook(&ExitHook);
        Kernel::SysExit(42, nullptr);
        Check(false, "standalone real SysExit unexpectedly returned");
    }
    else Check(false, "unknown route");
    Check(RemoveVectoredExceptionHandler(registration) != 0, "remove kernel VEH");
    Kernel::Legacy::SetExceptionTraceReader(nullptr);
    Kernel::Legacy::SetExceptionTimelineReader(nullptr);
    Memory::Shutdown();
    if (argc == 3) WriteSummary(argv[2], route);
    std::printf("VERIFIED kernel_trap %s: asserted real legacy route; diagnostic history injected\n", argv[1]);
    return 0;
}
