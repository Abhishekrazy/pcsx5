#include <pcsx5/runtime/child_process.h>
#include "child_process_validation.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <chrono>
#include <exception>
#include <new>
#include <string_view>

namespace pcsx5::runtime {
namespace {

process_error native_error(DWORD error) noexcept {
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
        error == ERROR_BAD_EXE_FORMAT) return process_error::unavailable;
    if (error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY)
        return process_error::out_of_memory;
    return process_error::host_failure;
}

std::expected<std::wstring, process_error> decode_argument(std::string_view input) {
    if (input.empty()) return std::wstring{};
    // Shared validation bounds input to 8192 bytes, so this conversion is exact.
    const auto count = static_cast<int>(input.size());
    const auto required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        input.data(), count, nullptr, 0);
    if (required == 0) return std::unexpected(process_error::invalid_argument);
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), count,
        result.data(), required) != required)
        return std::unexpected(process_error::host_failure);
    return result;
}

void append_argument(std::wstring& command, std::wstring_view argument) {
    command += L" \"";
    std::size_t slashes{};
    for (const auto character : argument) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        // MS CRT: 2n+1 slashes before a quote encode n slashes and a literal quote.
        command.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        command += character;
        slashes = 0;
    }
    // Escape trailing slashes so they do not consume the closing quote.
    command.append(slashes * 2, L'\\');
    command += L'"';
}

class child_owner final {
public:
    explicit child_owner(PROCESS_INFORMATION information) noexcept : information_(information) {}
    ~child_owner() {
        if (!reaped_) terminate_and_reap();
        if (!CloseHandle(information_.hThread)) std::terminate();
        if (!CloseHandle(information_.hProcess)) std::terminate();
    }
    child_owner(const child_owner&) = delete;
    child_owner& operator=(const child_owner&) = delete;
    child_owner(child_owner&&) = delete;
    child_owner& operator=(child_owner&&) = delete;

    [[nodiscard]] HANDLE process() const noexcept { return information_.hProcess; }
    void mark_reaped() noexcept { reaped_ = true; }
    void terminate_and_reap() noexcept {
        if (reaped_) return;
        // Only the handle returned by our successful CreateProcessW is targeted.
        if (!TerminateProcess(process(), 1) && WaitForSingleObject(process(), 0) != WAIT_OBJECT_0)
            std::terminate();
        if (WaitForSingleObject(process(), INFINITE) != WAIT_OBJECT_0) std::terminate();
        reaped_ = true;
    }

private:
    PROCESS_INFORMATION information_{};
    bool reaped_{};
};

process_run_result await_child(child_owner& owner, std::uint32_t timeout_ms,
    std::stop_token cancellation) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    DWORD wait_ms{};
    for (;;) {
        const auto observed = WaitForSingleObject(owner.process(), wait_ms);
        if (observed == WAIT_OBJECT_0) {
            owner.mark_reaped();
            DWORD code{};
            if (!GetExitCodeProcess(owner.process(), &code))
                return std::unexpected(process_error::host_failure);
            return process_result{process_outcome::exited, code};
        }
        if (observed != WAIT_TIMEOUT) return std::unexpected(process_error::host_failure);
        if (cancellation.stop_requested()) {
            owner.terminate_and_reap();
            return process_result{process_outcome::cancelled, 0};
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            owner.terminate_and_reap();
            return process_result{process_outcome::timed_out, 0};
        }
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
        wait_ms = static_cast<DWORD>(remaining < 5 ? remaining : 5);
    }
}

} // namespace

process_run_result run_windows_child(const std::filesystem::path& executable,
    std::span<const std::string> arguments, std::uint32_t timeout_ms,
    std::stop_token cancellation) noexcept {
    if (!detail::valid_child_request(executable, arguments, timeout_ms))
        return std::unexpected(process_error::invalid_argument);
    try {
        const auto& path = executable.native();
        // argv[0] uses CRT's distinct quoted-program-name rule. Quotes are not
        // valid Windows file-name characters and cannot occur inside this token.
        if (path.find(L'"') != std::wstring::npos)
            return std::unexpected(process_error::invalid_argument);
        std::wstring command = L"\"" + path + L"\"";
        for (const auto& argument : arguments) {
            const auto decoded = decode_argument(argument);
            if (!decoded) return std::unexpected(decoded.error());
            append_argument(command, *decoded);
        }
        // The shared 8192-unit bound plus quoting fits CreateProcessW's 32767
        // units including its terminator. All encoding validation precedes stop.
        if (cancellation.stop_requested()) return process_result{process_outcome::cancelled, 0};
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION information{};
        if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information))
            return std::unexpected(native_error(GetLastError()));
        child_owner owner(information);
        return await_child(owner, timeout_ms, cancellation);
    } catch (const std::bad_alloc&) {
        return std::unexpected(process_error::out_of_memory);
    } catch (...) {
        return std::unexpected(process_error::host_failure);
    }
}

} // namespace pcsx5::runtime
