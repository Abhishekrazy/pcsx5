#include <pcsx5/execution/native_step.h>
#include "native_step_testing.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <chrono>
#include <exception>
#include <new>
#include <string>
#include <thread>

namespace pcsx5::execution {
namespace {
using clock_type = std::chrono::steady_clock;

native_error translate_error(DWORD error) noexcept {
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
        error == ERROR_BAD_EXE_FORMAT || error == ERROR_ACCESS_DENIED ||
        error == ERROR_INVALID_ADDRESS) return native_error::unavailable;
    if (error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY)
        return native_error::out_of_memory;
    return native_error::host_failure;
}

void close_file(HANDLE handle) noexcept {
    if (handle && handle != INVALID_HANDLE_VALUE && !CloseHandle(handle)) std::terminate();
}

// Debug-event process/thread handles are closed by ContinueDebugEvent(EXIT_*),
// not by us. PROCESS_INFORMATION handles are separately owned. Defensively
// duplicate an alias if a provider returns the same handle in both structures.
void separate_handle(HANDLE& owned, HANDLE event_handle) noexcept {
    if (owned != event_handle) return;
    HANDLE replacement{};
    if (!DuplicateHandle(GetCurrentProcess(), owned, GetCurrentProcess(),
        &replacement, 0, FALSE, DUPLICATE_SAME_ACCESS)) std::terminate();
    owned = replacement;
}

class debug_child final {
public:
    explicit debug_child(PROCESS_INFORMATION information) noexcept : information_(information) {}
    debug_child(const debug_child&) = delete;
    debug_child& operator=(const debug_child&) = delete;
    debug_child(debug_child&&) = delete;
    debug_child& operator=(debug_child&&) = delete;
    ~debug_child() {
        dispose();
        close_file(information_.hThread);
        close_file(information_.hProcess);
    }
    HANDLE process() const noexcept { return information_.hProcess; }
    HANDLE thread() const noexcept { return information_.hThread; }
    DWORD pid() const noexcept { return information_.dwProcessId; }
    DWORD tid() const noexcept { return information_.dwThreadId; }

    bool wait(DWORD milliseconds, DWORD& error) noexcept {
        if (pending_) std::terminate();
        if (!WaitForDebugEvent(&event_, milliseconds)) { error = GetLastError(); return false; }
        pending_ = true;
        if (event_.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            separate_handle(information_.hProcess, event_.u.CreateProcessInfo.hProcess);
            separate_handle(information_.hThread, event_.u.CreateProcessInfo.hThread);
            close_file(event_.u.CreateProcessInfo.hFile);
        } else if (event_.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
            close_file(event_.u.LoadDll.hFile);
        }
        return true;
    }
    const DEBUG_EVENT& event() const noexcept { return event_; }
    bool resume() noexcept {
        if (!pending_) std::terminate();
        if (!ContinueDebugEvent(event_.dwProcessId, event_.dwThreadId, DBG_CONTINUE)) return false;
        pending_ = false;
        if (event_.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT && event_.dwProcessId == pid())
            exited_ = true;
        return true;
    }

private:
    void dispose() noexcept {
        if (!exited_) {
            // Do not resume an observed guest fault/step before requesting death.
            // No guest stack unwinding or guest exception dispatch is involved.
            if (!TerminateProcess(process(), 137) && WaitForSingleObject(process(), 0) != WAIT_OBJECT_0)
                std::terminate();
            if (pending_ && !resume()) std::terminate();
            // Cleanup is mandatory even after the execution deadline. A broken
            // debugger cannot silently abandon a stopped live child.
            const auto cleanup_deadline = clock_type::now() + std::chrono::seconds(10);
            while (!exited_) {
                DWORD error{};
                if (!wait(10, error)) {
                    if (error != ERROR_SEM_TIMEOUT || clock_type::now() >= cleanup_deadline)
                        std::terminate();
                    continue;
                }
                if (event_.dwProcessId != pid() || !resume()) std::terminate();
            }
        }
        if (WaitForSingleObject(process(), 10000) != WAIT_OBJECT_0) std::terminate();
    }
    PROCESS_INFORMATION information_{};
    DEBUG_EVENT event_{};
    bool pending_{};
    bool exited_{};
};

std::expected<void, native_error> install(debug_child& child, const native_request& request,
    testing::native_failure_stage stage, bool* reached) noexcept {
    auto* const code_address = reinterpret_cast<void*>(native_code_base);
    auto* const data_address = reinterpret_cast<void*>(native_data_base);
    // MEM_RESERVE fails on a collision; never commit into an unowned mapping.
    if (VirtualAllocEx(child.process(), code_address, native_page_bytes,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != code_address)
        return std::unexpected(translate_error(GetLastError()));
    if (VirtualAllocEx(child.process(), data_address, native_page_bytes,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != data_address)
        return std::unexpected(translate_error(GetLastError()));
    SIZE_T count{};
    if (!WriteProcessMemory(child.process(), code_address, request.code.data(), request.code.size(), &count) ||
        count != request.code.size()) return std::unexpected(native_error::host_failure);
    if (!WriteProcessMemory(child.process(), data_address, request.data.data(), request.data.size(), &count) ||
        count != request.data.size()) return std::unexpected(native_error::host_failure);
    DWORD old_protection{};
    if (!VirtualProtectEx(child.process(), code_address, native_page_bytes, PAGE_EXECUTE_READ, &old_protection) ||
        !FlushInstructionCache(child.process(), code_address, native_page_bytes))
        return std::unexpected(native_error::host_failure);
    if (stage == testing::native_failure_stage::after_images) {
        if (reached) *reached = true;
        return std::unexpected(native_error::host_failure);
    }
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!GetThreadContext(child.thread(), &context)) return std::unexpected(native_error::host_failure);
    const auto& r = request.initial.gpr;
    context.Rax=r[0]; context.Rcx=r[1]; context.Rdx=r[2]; context.Rbx=r[3];
    context.Rsp=r[4]; context.Rbp=r[5]; context.Rsi=r[6]; context.Rdi=r[7];
    context.R8=r[8]; context.R9=r[9]; context.R10=r[10]; context.R11=r[11];
    context.R12=r[12]; context.R13=r[13]; context.R14=r[14]; context.R15=r[15];
    context.Rip=request.initial.rip;
    // Keep OS-owned control flags from the stopped host context. Only modeled
    // arithmetic flags are supplied by the request; TF is debugger transport.
    context.EFlags = (context.EFlags & ~static_cast<DWORD>(arithmetic_flags | 0x10400U)) |
        static_cast<DWORD>(request.initial.rflags) | 0x100U;
    if (!SetThreadContext(child.thread(), &context)) return std::unexpected(native_error::host_failure);
    if (stage == testing::native_failure_stage::after_context) {
        if (reached) *reached = true;
        return std::unexpected(native_error::host_failure);
    }
    return {};
}

native_result observe(debug_child& child, const native_request& request) noexcept {
    const auto& exception = child.event().u.Exception.ExceptionRecord;
    native_snapshot snapshot{};
    switch (exception.ExceptionCode) {
    case EXCEPTION_SINGLE_STEP: snapshot.stop = native_stop::stepped; break;
    case EXCEPTION_BREAKPOINT: snapshot.stop = native_stop::breakpoint; break;
    case EXCEPTION_ACCESS_VIOLATION:
        if (exception.NumberParameters < 2) return std::unexpected(native_error::host_failure);
        snapshot.stop = native_stop::memory_fault;
        snapshot.fault_address = exception.ExceptionInformation[1];
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION: snapshot.stop = native_stop::illegal_instruction; break;
    default: return std::unexpected(native_error::host_failure);
    }
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!GetThreadContext(child.thread(), &context)) return std::unexpected(native_error::host_failure);
    snapshot.state.gpr = {context.Rax,context.Rcx,context.Rdx,context.Rbx,
        context.Rsp,context.Rbp,context.Rsi,context.Rdi,context.R8,context.R9,
        context.R10,context.R11,context.R12,context.R13,context.R14,context.R15};
    snapshot.state.rip = context.Rip;
    snapshot.state.rflags = (context.EFlags & arithmetic_flags) | 2;
    snapshot.state.known_flags = request.initial.known_flags;
    SIZE_T count{};
    if (!ReadProcessMemory(child.process(), reinterpret_cast<const void*>(native_data_base),
        snapshot.data.data(), snapshot.data.size(), &count) || count != snapshot.data.size())
        return std::unexpected(native_error::host_failure);
    return snapshot;
}

native_result drive(debug_child& child, const native_request& request,
    clock_type::time_point deadline, testing::native_failure_stage stage, bool* reached) noexcept {
    bool created{}, installed{};
    struct bootstrap_thread { DWORD id{}; HANDLE handle{}; };
    std::array<bootstrap_thread,64> bootstrap_threads{};
    for (;;) {
        if (clock_type::now() >= deadline) return std::unexpected(native_error::timed_out);
        DWORD error{};
        if (!child.wait(1, error)) {
            if (error == ERROR_SEM_TIMEOUT) continue;
            return std::unexpected(native_error::host_failure);
        }
        const auto& event = child.event();
        if (event.dwProcessId != child.pid()) return std::unexpected(native_error::host_failure);
        switch (event.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            if (created || event.dwThreadId != child.tid()) return std::unexpected(native_error::host_failure);
            created = true;
            break;
        case EXCEPTION_DEBUG_EVENT:
            if (!created || event.dwThreadId != child.tid() || !event.u.Exception.dwFirstChance)
                return std::unexpected(native_error::host_failure);
            if (installed) {
                if (stage == testing::native_failure_stage::before_snapshot) {
                    if (reached) *reached = true;
                    return std::unexpected(native_error::host_failure);
                }
                return observe(child, request);
            }
            if (event.u.Exception.ExceptionRecord.ExceptionCode != EXCEPTION_BREAKPOINT)
                return std::unexpected(native_error::host_failure);
            // Windows may use loader workers before its initial breakpoint.
            // They may run during bootstrap, but no other thread may execute
            // while the modeled instruction is stepped. Event handles remain
            // OS-owned until the corresponding EXIT event is continued.
            for (const auto& thread : bootstrap_threads)
                if (thread.handle && SuspendThread(thread.handle) == static_cast<DWORD>(-1))
                    return std::unexpected(native_error::host_failure);
            if (const auto result = install(child, request, stage, reached); !result) return std::unexpected(result.error());
            if (stage == testing::native_failure_stage::hold_after_context) {
                if (reached) *reached = true;
                // Keep the actual debug event pending until the real deadline.
                while (clock_type::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return std::unexpected(native_error::timed_out);
            }
            installed = true;
            break;
        case CREATE_THREAD_DEBUG_EVENT: {
            if (!created || installed) return std::unexpected(native_error::host_failure);
            bool retained{};
            for (auto& thread : bootstrap_threads) {
                if (thread.handle) continue;
                thread = {event.dwThreadId, event.u.CreateThread.hThread};
                retained = true;
                break;
            }
            if (!retained || !event.u.CreateThread.hThread) return std::unexpected(native_error::host_failure);
            break;
        }
        case EXIT_THREAD_DEBUG_EVENT: {
            if (installed || event.dwThreadId == child.tid()) return std::unexpected(native_error::host_failure);
            bool found{};
            for (auto& thread : bootstrap_threads) {
                if (thread.handle && thread.id == event.dwThreadId) {
                    thread = {};
                    found = true;
                    break;
                }
            }
            if (!found) return std::unexpected(native_error::host_failure);
            break;
        }
        case LOAD_DLL_DEBUG_EVENT:
        case UNLOAD_DLL_DEBUG_EVENT:
        case OUTPUT_DEBUG_STRING_EVENT:
            if (installed) return std::unexpected(native_error::host_failure);
            break;
        case EXIT_PROCESS_DEBUG_EVENT:
            if (!child.resume()) return std::unexpected(native_error::host_failure);
            return std::unexpected(native_error::host_failure);
        default:
            // Arbitrary debugger events are not part of this probe contract.
            return std::unexpected(native_error::host_failure);
        }
        if (!child.resume()) return std::unexpected(native_error::host_failure);
    }
}
} // namespace

native_result testing::step_windows_native_injected(const std::filesystem::path& helper,
    const native_request& request, std::uint32_t timeout_ms, native_failure_stage stage, bool* reached) noexcept {
    if (reached) *reached = false;
    if (stage != native_failure_stage::none && stage != native_failure_stage::after_images &&
        stage != native_failure_stage::after_context && stage != native_failure_stage::before_snapshot &&
        stage != native_failure_stage::hold_after_context)
        return std::unexpected(native_error::invalid);
    const auto& path = helper.native();
    if (!helper.is_absolute() || path.empty() || path.size() > 8192 ||
        path.find(L'\0') != std::wstring::npos || path.find(L'"') != std::wstring::npos ||
        timeout_ms == 0 || timeout_ms > 60000 || (request.initial.rflags & 2) == 0 ||
        (request.initial.rflags & ~(arithmetic_flags | 2ULL)) != 0 ||
        (request.initial.known_flags & ~arithmetic_flags) != 0)
        return std::unexpected(native_error::invalid);
    try {
        std::wstring command = L"\"" + path + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION information{};
        if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE,
            DEBUG_ONLY_THIS_PROCESS | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information))
            return std::unexpected(translate_error(GetLastError()));
        const auto deadline = clock_type::now() + std::chrono::milliseconds(timeout_ms);
        debug_child child(information);
        return drive(child, request, deadline, stage, reached);
    } catch (const std::bad_alloc&) { return std::unexpected(native_error::out_of_memory); }
    catch (...) { return std::unexpected(native_error::host_failure); }
}
native_result step_windows_native(const std::filesystem::path& helper,
    const native_request& request, std::uint32_t timeout_ms) noexcept {
    return testing::step_windows_native_injected(helper,request,timeout_ms,testing::native_failure_stage::none);
}
} // namespace pcsx5::execution
