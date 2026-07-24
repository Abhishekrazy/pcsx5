#pragma once
//
// C API seam for the pcsx5 emulator core (pcsx5_core.dll).
//
// The same entry points back both consumers:
//   - pcsx5_cli.exe (thin argv shim, in_proc = 0)
//   - the WPF app hosting the core in-process (in_proc = 1)
//
// Call sequence for a game session:
//   pcsx5_init(options, log_cb, user) -> pcsx5_load(eboot) -> pcsx5_run(...)
//   -> pcsx5_shutdown()
// pcsx5_run() blocks until the guest exits or pcsx5_stop() is called from
// another thread; it owns the GLFW window/message loop and must therefore be
// called from the same thread that called pcsx5_init().
//
// All functions are thread-compatible with a single session at a time: the
// core globals are one-shot, so a new session requires a full shutdown of
// the previous one.
//

#ifdef _WIN32
#define PCSX5_API __declspec(dllexport)
#else
#define PCSX5_API
#endif

extern "C" {

struct pcsx5_options {
    const char* config_dir;             // NULL -> "pcsx5_config"
    const char* crash_dir;              // NULL -> "pcsx5_crash"
    const char* log_file;               // NULL -> no extra file mirror
    const char* title_id;               // NULL/"" -> no per-title overrides
    const char* report_path;            // NULL/"" -> no JSON compat summary
    const char* regression_report_path; // NULL/"" -> no regression report
    int strict_imports;                 // non-zero: unresolved imports = hard failure
    int embed;                          // non-zero: render window starts hidden
    int in_proc;                        // non-zero: hosted inside another process
                                        // (skip process-wide crash/SEH hooks,
                                        //  report the HWND via callback)
};

// Log record forwarded from the core.  `level` and `category` are the
// LogLevel / LogCategory enum values (see src/common/log.h) as ints.
typedef void (*pcsx5_log_cb)(int level, int category, const char* msg, void* user);

// Presentation-window handle notification (in-proc mode only; the CLI keeps
// the stdout PCSX5_WINDOW_HANDLE= line).  Fires once the GLFW window exists —
// during pcsx5_init() if the callback was registered early, otherwise at the
// start of pcsx5_run().
typedef void (*pcsx5_window_cb)(unsigned long long hwnd, void* user);

// Initialize configuration, logging, and all emulator subsystems.
// Returns 0 on success, non-zero on failure.
PCSX5_API int  pcsx5_init(const pcsx5_options* options, pcsx5_log_cb log_cb, void* log_user);

// Load the main ELF/SELF module (and its PRX dependencies).  Returns 0 on
// success, non-zero on failure.
PCSX5_API int  pcsx5_load(const char* eboot_path);

// Execute the guest.  Blocks (running the window/message loop on the calling
// thread) until the guest finishes or pcsx5_stop() is requested.  Returns
// the guest exit code (>= 0), 3 on strict-import failure, or -1 on error.
PCSX5_API int  pcsx5_run(pcsx5_window_cb window_cb, void* window_user);

// Ask the running guest to stop gracefully.  Safe to call from any thread;
// the guest observes the flag on its next HLE dispatch and pcsx5_run()
// returns after the 5-second force-stop timeout.
PCSX5_API void pcsx5_stop(void);

// Immediately terminate the guest thread (hard kill).  The guest does not
// get a chance to clean up.  Use only when pcsx5_stop() does not respond.
PCSX5_API void pcsx5_force_stop(void);

// Pause / resume emulator execution loop. Safe to call from any thread.
PCSX5_API void pcsx5_pause(void);
PCSX5_API void pcsx5_resume(void);

// Tear down all subsystems and persist the run summary/reports.  Must be
// called after pcsx5_run() returns (or after a failed load) before the
// process exits or a new session starts.
PCSX5_API void pcsx5_shutdown(void);

// Standalone PKG extraction (no emulator startup).  Returns 0 on success,
// non-zero on failure.
PCSX5_API int  pcsx5_extract_pkg(const char* pkg_path, const char* out_dir);

// Retrieve the last guest-crash error string for display in the frontend.
// Returns 0 on success (crash info written into `buf`), -1 if no crash
// has occurred.  `buf` is always NUL-terminated when buf_size > 0.
PCSX5_API int  pcsx5_get_last_error(char* buf, int buf_size);

}
