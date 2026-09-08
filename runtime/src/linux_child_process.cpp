#include <pcsx5/runtime/child_process.h>
#include "child_process_validation.h"

#include <cerrno>
#include <chrono>
#include <exception>
#include <new>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace pcsx5::runtime {
namespace {
class owned_child final {
public:
    explicit owned_child(pid_t pid) noexcept : pid_(pid) {}
    ~owned_child() { terminate_and_reap(); }
    owned_child(const owned_child&) = delete;
    owned_child& operator=(const owned_child&) = delete;
    owned_child(owned_child&&) = delete;
    owned_child& operator=(owned_child&&) = delete;

    [[nodiscard]] pid_t poll(int& status) noexcept {
        pid_t result{};
        do {
            result = ::waitpid(pid_, &status, WNOHANG);
        } while (result == -1 && errno == EINTR);
        // Do not signal a potentially recycled PID after ownership was lost.
        if (result == -1 && errno == ECHILD) std::terminate();
        if (result == pid_ && (WIFEXITED(status) || WIFSIGNALED(status))) pid_ = -1;
        return result;
    }

    void terminate_and_reap() noexcept {
        if (pid_ <= 0) return;
        int result{};
        do {
            result = ::kill(pid_, SIGKILL);
        } while (result == -1 && errno == EINTR);
        if (result == -1 && errno != ESRCH) std::terminate();
        int status{};
        pid_t waited{};
        do {
            waited = ::waitpid(pid_, &status, 0);
        } while (waited == -1 && errno == EINTR);
        if (waited != pid_ || (!WIFEXITED(status) && !WIFSIGNALED(status))) std::terminate();
        pid_ = -1;
    }

private:
    pid_t pid_;
};

process_error spawn_error(int error) noexcept {
    if (error == ENOMEM) return process_error::out_of_memory;
    if (error == ENOENT || error == ENOTDIR || error == EACCES || error == ENOEXEC)
        return process_error::unavailable;
    return process_error::host_failure;
}
} // namespace

process_run_result run_linux_child(const std::filesystem::path& executable,
    std::span<const std::string> arguments, std::uint32_t timeout_ms,
    std::stop_token cancellation) noexcept {
    if (!detail::valid_child_request(executable, arguments, timeout_ms))
        return std::unexpected(process_error::invalid_argument);
    if (cancellation.stop_requested()) return process_result{process_outcome::cancelled, 0};

    struct sigaction disposition {};
    if (::sigaction(SIGCHLD, nullptr, &disposition) != 0 ||
        disposition.sa_handler == SIG_IGN || (disposition.sa_flags & SA_NOCLDWAIT) != 0)
        return std::unexpected(process_error::host_failure);

    try {
        // Own writable argv strings, avoiding const casts at the POSIX boundary.
        std::vector<std::string> storage;
        storage.reserve(arguments.size() + 1);
        storage.push_back(executable.native());
        for (const auto& argument : arguments) storage.push_back(argument);
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (auto& argument : storage) argv.push_back(argument.data());
        argv.push_back(nullptr);

        pid_t pid{};
        const int error = ::posix_spawn(&pid, storage.front().c_str(), nullptr,
            nullptr, argv.data(), environ);
        if (error != 0) return std::unexpected(spawn_error(error));
        if (pid <= 0) std::terminate();
        owned_child child(pid);
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeout_ms);
        constexpr auto poll_interval = std::chrono::milliseconds(1);
        for (;;) {
            int status{};
            const auto waited = child.poll(status);
            if (waited == pid) {
                if (WIFEXITED(status)) return process_result{process_outcome::exited,
                    static_cast<std::uint32_t>(WEXITSTATUS(status))};
                if (WIFSIGNALED(status)) return process_result{process_outcome::signaled,
                    static_cast<std::uint32_t>(WTERMSIG(status))};
                return std::unexpected(process_error::host_failure);
            }
            if (waited == -1) return std::unexpected(process_error::host_failure);
            if (cancellation.stop_requested()) {
                child.terminate_and_reap();
                return process_result{process_outcome::cancelled, 0};
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                child.terminate_and_reap();
                return process_result{process_outcome::timed_out, 0};
            }
            std::this_thread::sleep_for(poll_interval);
        }
    } catch (const std::bad_alloc&) {
        return std::unexpected(process_error::out_of_memory);
    } catch (...) {
        return std::unexpected(process_error::host_failure);
    }
}
} // namespace pcsx5::runtime
