#define _CRT_SECURE_NO_WARNINGS
#include "core_api.h"
#include "gpu/gpu.h"
#include "gpu/input/input_backend.h"
#include "ui/button_layout.h"
#include "ipc/ipc_server.h"
#include "ipc/ipc_gpu_bridge.h"
#include <cstdio>
#include <filesystem>
#include <string>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

// Forward: input tester mode (D3.4).
static int RunInputTester();

namespace {

void PrintUsage() {
    std::printf("Usage:\n");
    std::printf("  pcsx5_cli.exe [--strict-imports] [--report=<path>] [--regression-report=<path>]\n");
    std::printf("          [--log-file=<path>] [--crash-dir=<path>]\n");
    std::printf("          [--config-dir=<path>] [--title-id=<id>] <path_to_eboot.bin_or_elf>\n");
    std::printf("  pcsx5_cli.exe --extract-pkg <file.pkg> <outdir>\n");
    std::printf("\nOptions:\n");
    std::printf("  --extract-pkg <pkg> <outdir> Extract a (fake-signed) PKG into <outdir> and exit.\n");
    std::printf("  --strict-imports             Fail (return non-zero) on unresolved imports.\n");
    std::printf("  --report=<path>              Write a JSON compatibility summary to <path>.\n");
    std::printf("  --regression-report=<path>   Write an aggregated markdown regression report.\n");
    std::printf("  --log-file=<path>            Mirror log output to <path>.\n");
    std::printf("  --crash-dir=<path>           Directory for crash-report bundles (default: pcsx5_crash).\n");
    std::printf("  --config-dir=<path>          Directory holding global.json + per-title overrides.\n");
    std::printf("  --title-id=<id>              PS5 title id (CUSAxxxxx) for per-title overrides and history.\n");
    std::printf("  --embed                      Create the render window hidden so the launcher UI can\n");
    std::printf("                               embed it (the window handle is always printed to stdout\n");
    std::printf("                               as PCSX5_WINDOW_HANDLE=<decimal HWND>).\n");
    std::printf("  --ipc-map=<name>             Shared memory file mapping name (IPC mode).\n");
    std::printf("  --ipc-pipe=<name>            Named pipe name (IPC mode).\n");
}

} // namespace

// Thin CLI shim over the pcsx5_core C API: parses argv into pcsx5_options and
// drives init -> load -> run -> shutdown.  All emulator phases live in
// core_api.cpp; this file only owns argument parsing and process exit codes.
int main(int argc, char* argv[]) {
    // Disable stdout/stderr buffering for instant logging on crash
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    // Add plugins/ to the DLL search path so pcsx5_core.dll can be found
    // when it lives next to the exe in a plugins subdirectory.
    {
        char exe_dir[MAX_PATH] = {};
        DWORD len = GetModuleFileNameA(nullptr, exe_dir, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::string dir = std::filesystem::path(exe_dir).parent_path().string();
            std::string plugins = dir + "\\plugins";
            if (std::filesystem::exists(plugins)) {
                SetDllDirectoryW(std::wstring(plugins.begin(), plugins.end()).c_str());
            }
        }
    }

    // Print emulator banner
    std::printf("\033[95m==================================================\n");
    std::printf("  pcsx5 - PlayStation 5 Emulator & Compatibility Layer\n");
    std::printf("==================================================\033[0m\n");

    // Parse options
    pcsx5_options options = {};
    std::string target_path;
    std::string crash_dir;
    std::string extract_pkg_path;
    std::string extract_pkg_outdir;
    std::string ipc_map_name;
    std::string ipc_pipe_name;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--extract-pkg") {
            if (i + 2 >= argc) {
                std::printf("--extract-pkg requires <file.pkg> and <outdir>.\n");
                PrintUsage();
                return 2;
            }
            extract_pkg_path   = argv[++i];
            extract_pkg_outdir = argv[++i];
        } else if (a == "--strict-imports") {
            options.strict_imports = 1;
        } else if (a.rfind("--report=", 0) == 0) {
            options.report_path = argv[i] + 9;
        } else if (a.rfind("--regression-report=", 0) == 0) {
            options.regression_report_path = argv[i] + 20;
        } else if (a.rfind("--log-file=", 0) == 0) {
            options.log_file = argv[i] + 11;
        } else if (a.rfind("--crash-dir=", 0) == 0) {
            crash_dir = a.substr(12);
        } else if (a.rfind("--config-dir=", 0) == 0) {
            options.config_dir = argv[i] + 13;
        } else if (a.rfind("--title-id=", 0) == 0) {
            options.title_id = argv[i] + 11;
        } else if (a == "--embed") {
            options.embed = 1;
        } else if (a.rfind("--ipc-map=", 0) == 0) {
            ipc_map_name = a.substr(10);
        } else if (a.rfind("--ipc-pipe=", 0) == 0) {
            ipc_pipe_name = a.substr(11);
        } else if (a == "-h" || a == "--help") {
            PrintUsage();
            return 0;
        } else if (a == "--test-input") {
            target_path = "--test-input";
        } else if (target_path.empty()) {
            target_path = a;
        } else {
            std::printf("Unknown argument: %s\n", a.c_str());
            PrintUsage();
            return 2;
        }
    }
    if (!crash_dir.empty()) options.crash_dir = crash_dir.c_str();

    // PKG-extraction mode: run standalone and exit (no emulator startup).
    if (!extract_pkg_path.empty()) {
        return pcsx5_extract_pkg(extract_pkg_path.c_str(), extract_pkg_outdir.c_str());
    }

    // Input tester mode: show the controller visualizer + all input backends.
    if (target_path == "--test-input") {
        return RunInputTester();
    }

    if (target_path.empty()) {
        // No game given (e.g. double-clicked): hand off to the launcher UI
        // (pcsx5.exe, the WPF app) when it sits next to this executable;
        // fall back to usage text.
        char module_path[MAX_PATH] = {};
        const DWORD len = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            const std::filesystem::path ui_path =
                std::filesystem::path(module_path).parent_path() / "pcsx5.exe";
            if (std::filesystem::exists(ui_path)) {
                std::printf("No game specified; launching pcsx5.exe ...\n");
                const HINSTANCE rc = ShellExecuteA(nullptr, "open", ui_path.string().c_str(),
                                                   nullptr, ui_path.parent_path().string().c_str(),
                                                   SW_SHOWNORMAL);
                return (reinterpret_cast<intptr_t>(rc) > 32) ? 0 : 1;
            }
        }
        PrintUsage();
        return 1;
    }

    int rc = pcsx5_init(&options, nullptr, nullptr);
    if (rc != 0) return rc;

    // IPC mode: connect to the frontend's shared memory + pipe.
    if (!ipc_map_name.empty() && !ipc_pipe_name.empty()) {
        if (!IPC::Initialize(ipc_map_name.c_str(), ipc_pipe_name.c_str())) {
            std::fprintf(stderr, "IPC initialization failed.\n");
            pcsx5_shutdown();
            return 1;
        }
        // Wire up GPU frame write to shared memory.
        GPU::IPC_SetWriteFrame(IPC::WriteFrame, IPC::IsConnected);
    }

    rc = pcsx5_load(target_path.c_str());
    if (rc == 0) {
        rc = pcsx5_run(nullptr, nullptr);
    }

    IPC::Shutdown();
    pcsx5_shutdown();
    return rc;
}

// D3.4: console-based input tester (replaces standalone dualsense_test).
// Polls all available input backends and prints live state once per second.
// Full visual version (ImGui ButtonLayout) requires access to GPU internals;
// use the existing debug overlay or one of the standalone tools for visuals.
static int RunInputTester() {
    // Create all input backends.
    static const int kMaxBackends = 4;
    InputBackend* backends[kMaxBackends] = {};
    const char* backend_names[kMaxBackends] = {"Keyboard", "XInput", "SDL", "DualSense"};
    backends[0] = InputBackend::Create("keyboard");
    backends[1] = InputBackend::Create("xinput");
    backends[2] = InputBackend::Create("sdl");
    backends[3] = InputBackend::Create("dualsense");

    int active = 0;
    for (int i = 0; i < kMaxBackends; ++i) {
        if (backends[i] && backends[i]->Initialize(0)) {
            std::printf("[%s] initialized\n", backend_names[i]);
            ++active;
        } else {
            delete backends[i];
            backends[i] = nullptr;
        }
    }
    if (active == 0) {
        std::fprintf(stderr, "No input backends available.\n");
        return 1;
    }

    std::printf("\nInput tester running — press Ctrl+C to exit.\n");
    std::printf("Polling %d backend(s) every second...\n\n", active);

    static const char* kButtonNames[] = {
        "Up","Down","Left","Right","L3","R3","L1","R1",
        "L2","R2","Cross","Circle","Square","Triangle",
        "Options","PS","Touchpad"
    };

    int frame = 0;
    while (true) {
        Sleep(250);  // 4 Hz polling

        // Poll backends and merge.
        uint32_t merged_buttons = 0;
        uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
        uint8_t l2 = 0, r2 = 0;
        int touch_count = 0;
        bool any_connected = false;

        for (int i = 0; i < kMaxBackends; ++i) {
            if (!backends[i]) continue;
            ControllerState s{};
            if (!backends[i]->Poll(s)) continue;
            if (!any_connected && s.connected) {
                any_connected = true;
                lx = s.left_x; ly = s.left_y;
                rx = s.right_x; ry = s.right_y;
                l2 = s.l2; r2 = s.r2;
                touch_count = s.touch_count;
                merged_buttons = s.buttons;
            } else if (s.connected) {
                // Later backends override.
                merged_buttons = s.buttons;
                lx = s.left_x; ly = s.left_y;
                rx = s.right_x; ry = s.right_y;
                l2 = s.l2; r2 = s.r2;
                touch_count = s.touch_count;
            }
        }

        // Print state.
        if (!any_connected) {
            if (frame % 4 == 0) std::printf("[%04d] No controller connected\n", ++frame);
            ++frame;
            continue;
        }

        // Build button string.
        char btn_str[128] = {};
        for (size_t i = 0; i < sizeof(kButtonNames)/sizeof(kButtonNames[0]); ++i) {
            if (merged_buttons & (1u << i)) {
                std::strcat(btn_str, kButtonNames[i]);
                std::strcat(btn_str, " ");
            }
        }

        ++frame;
        std::printf("[%04d] Buttons: %-40s | L2:%3d R2:%3d | LS:(%3d,%3d) RS:(%3d,%3d) | Touch:%d\n",
                    frame, btn_str, l2, r2, lx, ly, rx, ry, touch_count);
    }

    return 0;
}
