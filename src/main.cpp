#include "core_api.h"
#include <cstdio>
#include <filesystem>
#include <string>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

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
}

} // namespace

// Thin CLI shim over the pcsx5_core C API: parses argv into pcsx5_options and
// drives init -> load -> run -> shutdown.  All emulator phases live in
// core_api.cpp; this file only owns argument parsing and process exit codes.
int main(int argc, char* argv[]) {
    // Disable stdout/stderr buffering for instant logging on crash
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

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
            // Launcher UI embedding mode: the GPU window starts hidden; the UI
            // reparents it into its own window using the printed HWND.
            options.embed = 1;
        } else if (a == "-h" || a == "--help") {
            PrintUsage();
            return 0;
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

    rc = pcsx5_load(target_path.c_str());
    if (rc == 0) {
        rc = pcsx5_run(nullptr, nullptr);
    }

    pcsx5_shutdown();
    return rc;
}
