#pragma once

#include "ui/console.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace Ui {

// Out-of-process launcher.  Spawns the backend binary and pipes its
// stdout/stderr into a LogConsole in a background reader thread.
//
// Lifecycle:
//   ProcessRunner r;
//   r.SetConsole(&console);
//   r.Start(backend_path, {elf_path, "--title-id=..."});
//   ...
//   if (r.exited()) std::printf("exit code %d\n", r.exit_code());
//   r.Stop();  // terminates the child if still running
class ProcessRunner {
public:
    using OnExit = std::function<void(int exit_code)>;

    ProcessRunner();
    ~ProcessRunner();

    ProcessRunner(const ProcessRunner&) = delete;
    ProcessRunner& operator=(const ProcessRunner&) = delete;

    // Start the backend.  Returns true on successful spawn.  Each element
    // of `args` is a single argv (no shell parsing).
    bool Start(const std::string& backend,
               const std::vector<std::string>& args,
               LogConsole* sink,
               OnExit on_exit = {});

    // Cooperative stop: if the child is alive, terminate it.
    void Stop();

    void Pause();
    void Resume();

    bool running() const { return running_.load(); }
    bool paused() const { return paused_.load(); }
    int  exit_code() const { return exit_code_; }

private:
    void ReaderLoop();

    std::thread             reader_;
    std::atomic<bool>       running_{false};
    std::atomic<bool>       paused_{false};
    int                     exit_code_ = 0;
    LogConsole*             sink_ = nullptr;
    OnExit                  on_exit_;
#ifdef _WIN32
    void*                   child_handle_ = nullptr;   // HANDLE
    void*                   pipe_out_     = nullptr;   // HANDLE
    void*                   pipe_err_     = nullptr;   // HANDLE
#else
    int                     child_pid_    = -1;
    int                     pipe_out_     = -1;
    int                     pipe_err_     = -1;
#endif
};

}  // namespace Ui
