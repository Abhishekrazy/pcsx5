#include "ui/process.h"
#include "common/log.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace Ui {

ProcessRunner::ProcessRunner() = default;

ProcessRunner::~ProcessRunner() {
    Stop();
    if (reader_.joinable()) reader_.join();
}

#ifdef _WIN32

// Build a Windows command line that is safe to pass to CreateProcessA.
// Each argument is wrapped in quotes; embedded quotes are escaped.
static std::string BuildWinCmdLine(const std::string& exe,
                                   const std::vector<std::string>& args) {
    std::string s;
    auto append_quoted = [&](const std::string& a) {
        s += '"';
        for (char c : a) {
            if (c == '"' || c == '\\') {
                int backslashes = 0;
                while (c == '\\' && (backslashes == 0 || (a.data() + (&c - a.data()) > a.data()))) ++backslashes;
                s.append(backslashes, '\\');
                s += '\\';
                s += c;
            } else {
                s += c;
            }
        }
        s += '"';
    };
    append_quoted(exe);
    for (const auto& a : args) {
        s += ' ';
        append_quoted(a);
    }
    return s;
}

bool ProcessRunner::Start(const std::string& backend,
                          const std::vector<std::string>& args,
                          LogConsole* sink,
                          OnExit on_exit) {
    if (running_.load()) {
        LOG_WARN(General, "ProcessRunner: already running, refusing to start.");
        return false;
    }
    sink_     = sink;
    on_exit_  = std::move(on_exit);
    exit_code_ = 0;
    paused_.store(false);

    HANDLE h_out_read  = nullptr;
    HANDLE h_out_write = nullptr;
    HANDLE h_err_read  = nullptr;
    HANDLE h_err_write = nullptr;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    if (!CreatePipe(&h_out_read, &h_out_write, &sa, 0) ||
        !CreatePipe(&h_err_read, &h_err_write, &sa, 0)) {
        LOG_ERROR(General, "ProcessRunner: CreatePipe failed.");
        return false;
    }
    SetHandleInformation(h_out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(h_err_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = h_out_write;
    si.hStdError  = h_err_write;
    si.hStdInput  = nullptr;

    PROCESS_INFORMATION pi = {};
    const std::string cmd = BuildWinCmdLine(backend, args);
    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');

    if (!CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr,
                        TRUE /*bInheritHandles*/, 0, nullptr, nullptr, &si, &pi)) {
        LOG_ERROR(General, "ProcessRunner: CreateProcessA failed for '%s' (err=%lu).",
                  backend.c_str(), GetLastError());
        CloseHandle(h_out_read); CloseHandle(h_out_write);
        CloseHandle(h_err_read); CloseHandle(h_err_write);
        return false;
    }
    // We don't need the write ends in the parent; closing them is what tells
    // ReadFile() to return when the child exits.
    CloseHandle(h_out_write);
    CloseHandle(h_err_write);

    child_handle_ = pi.hProcess;
    pipe_out_     = h_out_read;
    pipe_err_     = h_err_read;
    running_.store(true);
    reader_ = std::thread([this] { ReaderLoop(); });
    return true;
}

void ProcessRunner::ReaderLoop() {
    HANDLE handles[2] = { static_cast<HANDLE>(pipe_out_), static_cast<HANDLE>(pipe_err_) };
    char buf[4096];
    while (running_.load()) {
        bool read_something = false;
        for (int i = 0; i < 2; ++i) {
            DWORD avail = 0;
            if (PeekNamedPipe(handles[i], nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                DWORD to_read = (avail < sizeof(buf)) ? avail : sizeof(buf);
                DWORD n = 0;
                if (ReadFile(handles[i], buf, to_read, &n, nullptr) && n > 0) {
                    if (sink_) sink_->Append(buf, n);
                    read_something = true;
                }
            }
        }

        DWORD err0 = 0, err1 = 0;
        DWORD avail = 0;
        if (!PeekNamedPipe(handles[0], nullptr, 0, nullptr, &avail, nullptr)) {
            err0 = GetLastError();
        }
        if (!PeekNamedPipe(handles[1], nullptr, 0, nullptr, &avail, nullptr)) {
            err1 = GetLastError();
        }
        if (err0 == ERROR_BROKEN_PIPE && err1 == ERROR_BROKEN_PIPE) {
            break;
        }

        if (!read_something) {
            Sleep(15);
        }
    }
    // Drain any final bytes.
    for (HANDLE h : handles) {
        DWORD avail = 0;
        while (PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD to_read = (avail < sizeof(buf)) ? avail : sizeof(buf);
            DWORD n = 0;
            if (ReadFile(h, buf, to_read, &n, nullptr) && n > 0) {
                if (sink_) sink_->Append(buf, n);
            } else {
                break;
            }
        }
        CloseHandle(h);
    }
    DWORD code = 0;
    if (child_handle_) {
        // If it was suspended, resume it so it exits cleanly and we don't wait forever
        if (paused_.load()) {
            Resume();
        }
        WaitForSingleObject(child_handle_, INFINITE);
        GetExitCodeProcess(child_handle_, &code);
        CloseHandle(static_cast<HANDLE>(child_handle_));
        child_handle_ = nullptr;
    }
    exit_code_   = static_cast<int>(code);
    running_.store(false);
    if (on_exit_) on_exit_(exit_code_);
}

void ProcessRunner::Stop() {
    if (child_handle_) {
        if (paused_.load()) {
            Resume();
        }
        TerminateProcess(static_cast<HANDLE>(child_handle_), 1);
    }
}

void ProcessRunner::Pause() {
    if (!child_handle_ || paused_.load()) return;
    typedef LONG(NTAPI* pfnNtSuspendProcess)(HANDLE ProcessHandle);
    static pfnNtSuspendProcess NtSuspendProcess = (pfnNtSuspendProcess)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSuspendProcess");
    if (NtSuspendProcess) {
        NtSuspendProcess(static_cast<HANDLE>(child_handle_));
        paused_.store(true);
    }
}

void ProcessRunner::Resume() {
    if (!child_handle_ || !paused_.load()) return;
    typedef LONG(NTAPI* pfnNtResumeProcess)(HANDLE ProcessHandle);
    static pfnNtResumeProcess NtResumeProcess = (pfnNtResumeProcess)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtResumeProcess");
    if (NtResumeProcess) {
        NtResumeProcess(static_cast<HANDLE>(child_handle_));
        paused_.store(false);
    }
}

#else  // POSIX -------------------------------------------------------------

bool ProcessRunner::Start(const std::string& backend,
                          const std::vector<std::string>& args,
                          LogConsole* sink,
                          OnExit on_exit) {
    if (running_.load()) return false;
    sink_     = sink;
    on_exit_  = std::move(on_exit);
    exit_code_ = 0;
    paused_.store(false);

    int out_fds[2] = {-1, -1}, err_fds[2] = {-1, -1};
    if (pipe(out_fds) != 0 || pipe(err_fds) != 0) return false;

    const pid_t pid = fork();
    if (pid == 0) {
        // child
        dup2(out_fds[1], STDOUT_FILENO);
        dup2(err_fds[1], STDERR_FILENO);
        close(out_fds[0]); close(out_fds[1]);
        close(err_fds[0]); close(err_fds[1]);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(backend.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execv(backend.c_str(), argv.data());
        std::perror("execv");
        std::_exit(127);
    }
    if (pid < 0) {
        close(out_fds[0]); close(out_fds[1]);
        close(err_fds[0]); close(err_fds[1]);
        return false;
    }
    close(out_fds[1]); close(err_fds[1]);
    child_pid_ = pid;
    pipe_out_  = out_fds[0];
    pipe_err_  = err_fds[0];
    running_.store(true);
    reader_ = std::thread([this] { ReaderLoop(); });
    return true;
}

void ProcessRunner::ReaderLoop() {
    struct pollfd fds[2] = {{pipe_out_, POLLIN, 0}, {pipe_err_, POLLIN, 0}};
    char buf[4096];
    while (running_.load()) {
        int n = poll(fds, 2, 50);
        if (n <= 0) continue;
        for (int i = 0; i < 2; ++i) {
            if (fds[i].revents & POLLIN) {
                ssize_t r = read(fds[i].fd, buf, sizeof(buf));
                if (r > 0 && sink_) sink_->Append(buf, static_cast<std::size_t>(r));
                else if (r == 0) { fds[i].fd = -1; }
            }
        }
    }
    int status = 0;
    if (child_pid_ > 0) {
        if (paused_.load()) {
            Resume();
        }
        waitpid(child_pid_, &status, 0);
        child_pid_ = -1;
    }
    if (WIFEXITED(status)) exit_code_ = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) exit_code_ = 128 + WTERMSIG(status);
    running_.store(false);
    if (on_exit_) on_exit_(exit_code_);
}

void ProcessRunner::Stop() {
    if (child_pid_ > 0) {
        if (paused_.load()) {
            Resume();
        }
        kill(child_pid_, SIGTERM);
    }
}

void ProcessRunner::Pause() {
    if (child_pid_ > 0 && !paused_.load()) {
        kill(child_pid_, SIGSTOP);
        paused_.store(true);
    }
}

void ProcessRunner::Resume() {
    if (child_pid_ > 0 && paused_.load()) {
        kill(child_pid_, SIGCONT);
        paused_.store(false);
    }
}

#endif

}  // namespace Ui
