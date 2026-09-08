#include <pcsx5/execution/native_step.h>
#include "native_step_testing.h"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <new>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace pcsx5::execution {
namespace {
using clock_type = std::chrono::steady_clock;
constexpr int probe_unavailable = 77;
constexpr std::uint64_t transport_flags = 0x100 | 0x400 | 0x10000; // TF, DF, RF

class traced_child final {
public:
    explicit traced_child(pid_t pid) noexcept : pid_(pid) {}
    traced_child(const traced_child&) = delete;
    traced_child& operator=(const traced_child&) = delete;
    traced_child(traced_child&&) = delete;
    traced_child& operator=(traced_child&&) = delete;
    ~traced_child() {
        if (pid_ <= 0) return;
        int status{};
        // Detect already-reaped ownership before signaling a possibly reused ID.
        const auto checked = poll(status);
        if (pid_ <= 0) return;
        if (checked < 0) std::terminate();
        if (::kill(pid_, SIGKILL) != 0 && errno != ESRCH) std::terminate();
        pid_t waited{};
        do { waited = ::waitpid(pid_, &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited != pid_ || (!WIFEXITED(status) && !WIFSIGNALED(status))) std::terminate();
    }
    pid_t poll(int& status) noexcept {
        pid_t waited{};
        do { waited = ::waitpid(pid_, &status, WNOHANG | WUNTRACED); }
        while (waited < 0 && errno == EINTR);
        if (waited < 0 && errno == ECHILD) std::terminate();
        if (waited == pid_ && (WIFEXITED(status) || WIFSIGNALED(status))) pid_ = -1;
        return waited;
    }
private:
    pid_t pid_;
};

std::expected<int, native_error> wait_stop(traced_child& child, clock_type::time_point deadline) noexcept {
    for (;;) {
        int status{};
        const auto waited = child.poll(status);
        if (waited < 0) return std::unexpected(native_error::host_failure);
        if (waited > 0) {
            if (WIFSTOPPED(status)) return WSTOPSIG(status);
            if (WIFEXITED(status) && WEXITSTATUS(status) == probe_unavailable)
                return std::unexpected(native_error::unavailable);
            return std::unexpected(native_error::host_failure);
        }
        if (clock_type::now() >= deadline) return std::unexpected(native_error::timed_out);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
bool put_image(pid_t pid, std::uint64_t base,
    const std::array<std::byte, native_page_bytes>& image) noexcept {
    static_assert(sizeof(long) == 8 && native_page_bytes % sizeof(long) == 0);
    for (std::size_t offset = 0; offset < image.size(); offset += sizeof(long)) {
        std::uintptr_t word{};
        std::memcpy(&word, image.data() + offset, sizeof(word));
        if (::ptrace(PTRACE_POKEDATA, pid, reinterpret_cast<void*>(base + offset),
            reinterpret_cast<void*>(word)) != 0) return false;
    }
    return true;
}
bool get_image(pid_t pid, std::array<std::byte, native_page_bytes>& image) noexcept {
    for (std::size_t offset = 0; offset < image.size(); offset += sizeof(long)) {
        errno = 0;
        const long word = ::ptrace(PTRACE_PEEKDATA, pid,
            reinterpret_cast<void*>(native_data_base + offset), nullptr);
        if (word == -1 && errno != 0) return false;
        std::memcpy(image.data() + offset, &word, sizeof(word));
    }
    return true;
}
void set_state(user_regs_struct& regs, const cpu_state& state) noexcept {
    regs.rax = state.gpr[0]; regs.rcx = state.gpr[1]; regs.rdx = state.gpr[2]; regs.rbx = state.gpr[3];
    regs.rsp = state.gpr[4]; regs.rbp = state.gpr[5]; regs.rsi = state.gpr[6]; regs.rdi = state.gpr[7];
    regs.r8 = state.gpr[8]; regs.r9 = state.gpr[9]; regs.r10 = state.gpr[10]; regs.r11 = state.gpr[11];
    regs.r12 = state.gpr[12]; regs.r13 = state.gpr[13]; regs.r14 = state.gpr[14]; regs.r15 = state.gpr[15];
    regs.rip = state.rip;
    regs.orig_rax = ~0ULL; // Do not restart the helper's interrupted syscall.
    regs.eflags = (regs.eflags & ~(arithmetic_flags | transport_flags)) | state.rflags;
}
cpu_state get_state(const user_regs_struct& regs, std::uint64_t known) noexcept {
    cpu_state state{};
    state.gpr = {regs.rax, regs.rcx, regs.rdx, regs.rbx, regs.rsp, regs.rbp, regs.rsi, regs.rdi,
        regs.r8, regs.r9, regs.r10, regs.r11, regs.r12, regs.r13, regs.r14, regs.r15};
    state.rip = regs.rip; state.rflags = (regs.eflags & arithmetic_flags) | 2;
    state.known_flags = known;
    return state;
}
} // namespace

int linux_native_probe() noexcept {
    const int personality = ::personality(0xffffffffUL);
    if (personality < 0 || (personality & READ_IMPLIES_EXEC) != 0) return probe_unavailable;
    auto* const requested_code = reinterpret_cast<void*>(native_code_base);
    auto* const code = ::mmap(requested_code, native_page_bytes, PROT_READ | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (code == MAP_FAILED) return probe_unavailable;
    if (code != requested_code) { ::munmap(code, native_page_bytes); return probe_unavailable; }
    auto* const requested_data = reinterpret_cast<void*>(native_data_base);
    auto* const data = ::mmap(requested_data, native_page_bytes, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (data == MAP_FAILED || data != requested_data) {
        if (data != MAP_FAILED) ::munmap(data, native_page_bytes);
        ::munmap(code, native_page_bytes);
        return probe_unavailable;
    }
    // The tracer writes RX text using ptrace; neither mapping is ever RWX.
    const bool ready = ::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) == 0;
    if (ready) ::raise(SIGSTOP);
    ::munmap(data, native_page_bytes); ::munmap(code, native_page_bytes);
    return probe_unavailable;
}

native_result testing::step_linux_native_injected(const std::filesystem::path& helper,
    const native_request& request, std::uint32_t timeout_ms, native_failure_stage stage, bool* reached) noexcept {
    if (reached) *reached = false;
    if (stage != native_failure_stage::none && stage != native_failure_stage::after_images &&
        stage != native_failure_stage::after_context && stage != native_failure_stage::before_snapshot &&
        stage != native_failure_stage::hold_after_context)
        return std::unexpected(native_error::invalid);
    if (helper.empty() || !helper.is_absolute() || helper.native().find('\0') != std::string::npos ||
        timeout_ms == 0 || timeout_ms > 60000 ||
        (request.initial.rflags & ~(arithmetic_flags | 2)) != 0 ||
        (request.initial.rflags & 2) == 0 || (request.initial.known_flags & ~arithmetic_flags) != 0)
        return std::unexpected(native_error::invalid);
    struct sigaction disposition {};
    if (::sigaction(SIGCHLD, nullptr, &disposition) != 0 || disposition.sa_handler == SIG_IGN ||
        (disposition.sa_flags & SA_NOCLDWAIT) != 0) return std::unexpected(native_error::host_failure);
    try {
        auto executable = helper.native();
        std::array<char*, 2> argv{executable.data(), nullptr};
        pid_t pid{};
        const int spawned = ::posix_spawn(&pid, executable.c_str(), nullptr, nullptr, argv.data(), environ);
        if (spawned != 0) return std::unexpected(spawned == ENOMEM ? native_error::out_of_memory :
            (spawned == ENOENT || spawned == EACCES || spawned == ENOEXEC || spawned == ENOTDIR) ?
                native_error::unavailable : native_error::host_failure);
        if (pid <= 0) std::terminate();
        traced_child child(pid);
        const auto deadline = clock_type::now() + std::chrono::milliseconds(timeout_ms);
        const auto initial_stop = wait_stop(child, deadline);
        if (!initial_stop) return std::unexpected(initial_stop.error());
        if (*initial_stop != SIGSTOP) return std::unexpected(native_error::host_failure);
        if (::ptrace(PTRACE_SETOPTIONS, pid, nullptr,
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(PTRACE_O_EXITKILL))) != 0)
            return std::unexpected(native_error::unavailable);
        user_regs_struct regs{};
        if (::ptrace(PTRACE_GETREGS, pid, nullptr, &regs) != 0 ||
            !put_image(pid, native_code_base, request.code) || !put_image(pid, native_data_base, request.data))
            return std::unexpected(native_error::host_failure);
        if (stage == native_failure_stage::after_images) {
            if (reached) *reached = true;
            return std::unexpected(native_error::host_failure);
        }
        set_state(regs, request.initial);
        if (::ptrace(PTRACE_SETREGS, pid, nullptr, &regs) != 0)
            return std::unexpected(native_error::host_failure);
        if (stage == native_failure_stage::after_context) {
            if (reached) *reached = true;
            return std::unexpected(native_error::host_failure);
        }
        if (stage == native_failure_stage::hold_after_context) {
            if (reached) *reached = true;
            // Real child remains ptrace-stopped until the real deadline expires.
            while (clock_type::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return std::unexpected(native_error::timed_out);
        }
        if (::ptrace(PTRACE_SINGLESTEP, pid, nullptr, nullptr) != 0)
            return std::unexpected(native_error::host_failure);
        const auto stopped = wait_stop(child, deadline);
        if (!stopped) return std::unexpected(stopped.error());
        if (stage == native_failure_stage::before_snapshot) {
            if (reached) *reached = true;
            return std::unexpected(native_error::host_failure);
        }
        siginfo_t info{};
        native_snapshot snapshot{};
        if (::ptrace(PTRACE_GETSIGINFO, pid, nullptr, &info) != 0 || info.si_signo != *stopped ||
            ::ptrace(PTRACE_GETREGS, pid, nullptr, &regs) != 0 || !get_image(pid, snapshot.data))
            return std::unexpected(native_error::host_failure);
        if (*stopped == SIGTRAP && info.si_code == TRAP_TRACE) snapshot.stop = native_stop::stepped;
        else if (*stopped == SIGTRAP && (info.si_code == TRAP_BRKPT || info.si_code == SI_KERNEL))
            snapshot.stop = native_stop::breakpoint;
        else if (*stopped == SIGSEGV && info.si_code > 0) {
            snapshot.stop = native_stop::memory_fault;
            snapshot.fault_address = reinterpret_cast<std::uintptr_t>(info.si_addr);
        } else if (*stopped == SIGILL && info.si_code > 0) snapshot.stop = native_stop::illegal_instruction;
        else return std::unexpected(native_error::host_failure);
        snapshot.state = get_state(regs, request.initial.known_flags);
        return snapshot;
    } catch (const std::bad_alloc&) { return std::unexpected(native_error::out_of_memory); }
    catch (...) { return std::unexpected(native_error::host_failure); }
}
native_result step_linux_native(const std::filesystem::path& helper,
    const native_request& request, std::uint32_t timeout_ms) noexcept {
    return testing::step_linux_native_injected(helper,request,timeout_ms,testing::native_failure_stage::none);
}
} // namespace pcsx5::execution
