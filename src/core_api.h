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
    const char* play_input_path;        // NULL/"" -> no input replay
    const char* record_input_path;      // NULL/"" -> no input recording
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

// ---------------------------------------------------------------------------
// Controller (DualSense) state for a frontend.  ADDITIVE ABI, 2026-09-06:
// these exports are new and nothing above them changed.  The shell used to
// read the pad through its own C# HID implementation, which competed with the
// core's reader for the same device; these let it read the core's instead.
//
// The struct is plain data with a fixed layout, so the C# mirror in
// CoreBridge.cs must match it field for field.  `struct_size` lets each side
// detect a mismatch instead of silently reading garbage.
// ---------------------------------------------------------------------------
typedef struct pcsx5_pad_touch {
    unsigned short x;       // 0..1919
    unsigned short y;       // 0..941
    unsigned char  id;      // firmware-assigned finger id
    unsigned char  active;  // 1 while the finger contacts the pad
} pcsx5_pad_touch;

typedef struct pcsx5_pad_state {
    unsigned int   struct_size;         // sizeof(pcsx5_pad_state), set by the core
    unsigned char  connected;
    unsigned char  bluetooth;           // 1 Bluetooth, 0 USB
    unsigned char  reserved0[2];
    unsigned int   buttons;             // SCE_PAD bitmask
    unsigned char  lx, ly, rx, ry;      // 0..255, 128 centred
    unsigned char  l2, r2;              // 0..255
    unsigned char  touch_count;         // 0..2
    unsigned char  reserved1;
    pcsx5_pad_touch touch[2];
    float          accel[3];            // g, approx
    float          gyro[3];             // rad/s, approx
    unsigned char  battery_level;       // 0..100
    unsigned char  battery_charging;
    unsigned char  battery_full;
    unsigned char  headphone;
    unsigned char  mic_jack;
    unsigned char  mic_muted;
    unsigned char  usb_data;
    unsigned char  usb_power;
    unsigned char  trigger_feedback[2];
    unsigned char  reserved2[2];
} pcsx5_pad_state;

typedef struct pcsx5_pad_firmware {
    unsigned int   struct_size;
    unsigned char  valid;
    unsigned char  reserved0[3];
    char           build_date[12];
    char           build_time[9];
    unsigned char  reserved1[3];
    unsigned short firmware_type;
    unsigned short software_series;
    unsigned int   hardware_info;       // low 16: model revision; bits 8..15: generation
    unsigned int   main_version;        // (v>>24).(v>>16 & 0xFF).(v & 0xFFFF)
    unsigned short update_version;      // X.X hex nibbles
    unsigned short reserved2;
    unsigned int   sbl_version;         // as main
    unsigned int   dsp_version;         // %04X_%04X
    unsigned int   mcu_dsp_version;     // as main
} pcsx5_pad_firmware;

// Number of controllers the core can currently stream.  0 or 1 today; the
// reader streams only the first enumerated pad until multi-controller support
// lands.  Starts the reader if it is not running.
PCSX5_API int  pcsx5_pad_count(void);

// Latest state for controller `index`.  Returns 0 and fills `out` on success,
// -1 for an index the core is not streaming, -2 if `out` is NULL or its
// struct_size does not match the core's.  `out->struct_size` must be set by
// the caller to sizeof(pcsx5_pad_state) before the call.
PCSX5_API int  pcsx5_pad_get_state(int index, pcsx5_pad_state* out);

// Firmware and hardware identity via feature report 0x20.  Same return
// convention as pcsx5_pad_get_state.  A synchronous HID read; a few ms.
PCSX5_API int  pcsx5_pad_get_firmware(int index, pcsx5_pad_firmware* out);

// Speaker volume (0..255) and preamp gain for the Bluetooth audio lane.
PCSX5_API void pcsx5_pad_set_audio_levels(unsigned char volume, unsigned char preamp);

// Built-in tests.  Each BLOCKS for about two seconds; call from a worker
// thread, never the UI thread.  Return 1 on success, 0 if the lane is
// unavailable (e.g. not on Bluetooth) or a write failed -- the log says which.
PCSX5_API int  pcsx5_pad_play_speaker_test(void);
PCSX5_API int  pcsx5_pad_play_haptics_test(void);

}
