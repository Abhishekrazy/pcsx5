#include "common/log.h"
#include "common/nid.h"
#include "common/types.h"
#include "loader/pkg.h"
#include "config/config.h"
#include "memory/memory.h"
#include "kernel/kernel.h"
#include "hle/hle.h"
#include "gpu/gpu.h"
#include "diagnostics/diagnostics.h"
#include "reports/reports.h"
#include "lua/lua_init.h"
#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <vector>
#include <cstdio>
#include <atomic>
#include <thread>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

namespace {

void PrintUsage() {
    std::printf("Usage:\n");
    std::printf("  pcsx5.exe [--strict-imports] [--report=<path>] [--regression-report=<path>]\n");
    std::printf("          [--log-file=<path>] [--crash-dir=<path>]\n");
    std::printf("          [--config-dir=<path>] [--title-id=<id>] <path_to_eboot.bin_or_elf>\n");
    std::printf("  pcsx5.exe --extract-pkg <file.pkg> <outdir>\n");
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

// Build a CompatSummary from the current run state.  title_id may be empty,
// in which case the target path is sanitised into a synthetic id.
Reports::CompatSummary BuildSummary(const std::string& target_path,
                                    const std::string& title_id,
                                    const std::string& status,
                                    const std::string& stage,
                                    double duration_ms) {
    Reports::CompatSummary s;
    s.title_id           = title_id;
    s.target             = target_path;
    s.status             = status;
    s.stage              = stage;
    s.duration_ms        = duration_ms;
    s.resolved_imports   = 0;       // filled in by caller via SetImportStats
    s.unresolved_imports = 0;
    s.timestamp_iso      = "";
    s.git_revision       = "";
    s.top_imports        = {};
    return s;
}

// Persist the per-run summary to all sinks requested by the user.  Called
// from both the success and failure paths so every run shows up in the
// regression database.
void PersistSummary(const Reports::CompatSummary& summary,
                    const std::string& report_path,
                    const std::string& regression_report_path) {
    if (!report_path.empty()) {
        std::string err;
        if (!Reports::WriteCompatSummary(report_path, summary, &err)) {
            LOG_WARN(General, "Failed to write compat summary: %s", err.c_str());
        }
        // Structured per-import JSON lives next to the compat summary.
        const std::filesystem::path import_json =
            std::filesystem::path(report_path).parent_path() / "import_report.json";
        if (!HLE::WriteImportReportJson(import_json.string())) {
            LOG_WARN(General, "Failed to write import report: %s", import_json.string().c_str());
        }
    }
    if (!summary.target.empty()) {
        Reports::AppendCompatHistory(ConfigService::Directory(), summary, nullptr);
    }
    if (!regression_report_path.empty()) {
        std::vector<Reports::RegressionEntry> entries;
        std::vector<Reports::CompatSummary> history =
            Reports::LoadCompatHistory(ConfigService::Directory(), summary.title_id, 32);
        // Drop the entry we just appended (its timestamp matches the current
        // run) so the regression baseline reflects the runs before this one.
        if (!history.empty() && history.front().timestamp_iso == summary.timestamp_iso) {
            history.erase(history.begin());
        }
        entries.push_back(Reports::EvaluateRegression(history, summary));
        std::string err;
        if (!Reports::WriteRegressionMarkdown(regression_report_path, entries, &err)) {
            LOG_WARN(General, "Failed to write regression report: %s", err.c_str());
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// --extract-pkg <file.pkg> <outdir>: extract every entry we can handle and
// print a short summary.  Returns 0 on success, non-zero on failure.
// ---------------------------------------------------------------------------
int ExtractPkgCli(const std::string& pkg_path, const std::string& out_dir) {
    Loader::PkgImage image;
    if (!Loader::ParsePkg(pkg_path, image)) {
        std::printf("error: failed to parse PKG: %s\n", pkg_path.c_str());
        return 1;
    }
    if (image.IsRetail()) {
        std::printf("warning: retail NPDRM PKG; encrypted entries will be skipped.\n");
    }

    size_t extracted = 0;
    size_t skipped = 0;
    for (const auto& entry : image.entries) {
        const std::filesystem::path dest =
            std::filesystem::path(out_dir) / Loader::PkgEntryPath(entry);
        std::error_code ec;
        std::filesystem::create_directories(dest.parent_path(), ec);
        if (Loader::ExtractPkgEntry(image, entry, dest.string())) {
            ++extracted;
        } else {
            ++skipped;
        }
    }

    std::printf("extracted %zu entr%s, skipped %zu -> %s\n",
                extracted, extracted == 1 ? "y" : "ies", skipped, out_dir.c_str());
    return extracted > 0 ? 0 : 1;
}

int main(int argc, char* argv[]) {
    // Disable stdout/stderr buffering for instant logging on crash
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    // Print emulator banner
    std::printf("\033[95m==================================================\n");
    std::printf("  pcsx5 - PlayStation 5 Emulator & Compatibility Layer\n");
    std::printf("==================================================\033[0m\n");

    // Parse options
    std::string target_path;
    std::string report_path;
    std::string regression_report_path;
    std::string log_file_path;
    std::string crash_dir = "pcsx5_crash";
    std::string config_dir;          // empty -> ./pcsx5_config
    std::string title_id;
    std::string extract_pkg_path;
    std::string extract_pkg_outdir;
    bool strict_imports = false;
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
            strict_imports = true;
        } else if (a.rfind("--report=", 0) == 0) {
            report_path = a.substr(9);
        } else if (a.rfind("--regression-report=", 0) == 0) {
            regression_report_path = a.substr(20);
        } else if (a.rfind("--log-file=", 0) == 0) {
            log_file_path = a.substr(11);
        } else if (a.rfind("--crash-dir=", 0) == 0) {
            crash_dir = a.substr(12);
        } else if (a.rfind("--config-dir=", 0) == 0) {
            config_dir = a.substr(13);
        } else if (a.rfind("--title-id=", 0) == 0) {
            title_id = a.substr(11);
        } else if (a == "--embed") {
            // Launcher UI embedding mode: the GPU window starts hidden; the UI
            // reparents it into its own window using the printed HWND.
            GPU::SetEmbeddedMode(true);
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

    // PKG-extraction mode: run standalone and exit (no emulator startup).
    if (!extract_pkg_path.empty()) {
        return ExtractPkgCli(extract_pkg_path, extract_pkg_outdir);
    }

    if (target_path.empty()) {
        // No game given (e.g. double-clicked): hand off to the launcher UI
        // when it sits next to this executable; fall back to usage text.
        char module_path[MAX_PATH] = {};
        const DWORD len = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            const std::filesystem::path ui_path =
                std::filesystem::path(module_path).parent_path() / "pcsx5_ui.exe";
            if (std::filesystem::exists(ui_path)) {
                std::printf("No game specified; launching pcsx5_ui.exe ...\n");
                const HINSTANCE rc = ShellExecuteA(nullptr, "open", ui_path.string().c_str(),
                                                   nullptr, ui_path.parent_path().string().c_str(),
                                                   SW_SHOWNORMAL);
                return (reinterpret_cast<intptr_t>(rc) > 32) ? 0 : 1;
            }
        }
        PrintUsage();
        return 1;
    }

    // Bring up the configuration service as the very first subsystem so every
    // other initialiser (logging, diagnostics, HLE) can read its settings.
    if (config_dir.empty()) config_dir = "pcsx5_config";
    ConfigService::Initialize(config_dir);
    GPU::SetBootStatus("Configuration loaded");

    // Apply the effective (global + per-title) configuration to the runtime.
    const ConfigService::Config& cfg = ConfigService::EffectiveFor(title_id);
    if (!title_id.empty() && ConfigService::ForTitle(title_id) != nullptr) {
        LOG_INFO(General, "Per-title config overrides active for %s (%s/titles/%s.json): "
                 "log level %s, strict imports %s",
                 title_id.c_str(), ConfigService::Directory().c_str(), title_id.c_str(),
                 cfg.logging.min_level == LogLevel::Debug ? "Debug" :
                 cfg.logging.min_level == LogLevel::Info  ? "Info"  :
                 cfg.logging.min_level == LogLevel::Warn  ? "Warn"  : "Error",
                 cfg.hle.strict_imports ? "on" : "off");
    }
    if (!cfg.logging.file_path.empty()) {
        LogConfig::SetFileOutput(cfg.logging.file_path, cfg.logging.file_append);
    } else if (!log_file_path.empty()) {
        LogConfig::SetFileOutput(log_file_path, /*append=*/false);
    }
    if (cfg.logging.json_output) LogConfig::SetJsonOutput(true);
    for (int i = 0; i < 6; ++i) {
        LogConfig::SetLevel(static_cast<LogCategory>(i), cfg.logging.min_level);
    }
    crash_dir = cfg.crash.bundle_dir.empty() ? crash_dir : cfg.crash.bundle_dir;
    strict_imports = strict_imports || cfg.hle.strict_imports;

    // Hand the audio output settings to libSceAudioOut (0 = Off / silent-paced).
    HLE::SetAudioOutConfig(cfg.audio.backend, cfg.audio.volume);

    // Install the crash-report handler early so any later crash (including
    // during subsystem init) is captured.
    Diagnostics::InstallCrashHandler(crash_dir);

    // Load the external NID name database (assets/nid_db.txt) if present.
    // Look next to the executable first, then in the CWD; missing file is
    // fine — the built-in table is always available.
    {
        std::vector<std::filesystem::path> nid_candidates;
        {
            std::error_code ec;
            const std::filesystem::path exe_abs =
                std::filesystem::absolute(argv[0], ec);
            if (!ec) {
                nid_candidates.push_back(exe_abs.parent_path() / "assets" / "nid_db.txt");
            }
        }
        nid_candidates.emplace_back("assets/nid_db.txt");
        bool nid_loaded = false;
        for (const auto& candidate : nid_candidates) {
            std::error_code ec;
            if (!std::filesystem::exists(candidate, ec)) continue;
            if (Common::LoadNidDatabase(candidate)) {
                LOG_INFO(General, "Loaded NID database: %s",
                         candidate.string().c_str());
            } else {
                LOG_WARN(General, "Failed to read NID database: %s",
                         candidate.string().c_str());
            }
            nid_loaded = true;
            break;
        }
        if (!nid_loaded) {
            LOG_INFO(General, "No NID database file found; using built-in table only.");
        }
        GPU::SetBootStatus("NID database ready");
    }

    if (!title_id.empty()) {
        LOG_INFO(General, "Active title: %s", title_id.c_str());
    }

    // 1. Initialize Subsystems — driven by the Lua subsystem registry.
    //    Falls back to the C++ default chain when Lua is unavailable.
    std::string init_error;
    if (!LuaInit::RunDefaultInit(&init_error)) {
        std::printf("FATAL: Failed to initialize subsystems: %s\n", init_error.c_str());
        return -1;
    }

    // Post-init configuration that requires the HLE subsystem to be ready.
    HLE::SetStrictImportMode(strict_imports);
    HLE::ResetRunStatistics();
    HLE::SetSaveDataTitleId(title_id);
    // Route guest /savedata0 file I/O to the same host dir the save-data HLE
    // uses (creates the dir if needed).
    Kernel::SetSaveDataDirectory(HLE::GetSaveDataDir());

    LOG_INFO(General, "All subsystems initialized successfully.");

    Reports::CompatSummary summary = BuildSummary(target_path, title_id,
                                                 "fail", "load", 0.0);

    auto t0 = std::chrono::steady_clock::now();

    // Configure PRX module resolution: the game's own sce_module/ directory
    // first, then the user-supplied firmware modules directory from config.
    {
        const std::string game_dir =
            std::filesystem::path(target_path).parent_path().string();
        Kernel::ConfigureModuleResolver(game_dir, cfg.loader.firmware_modules_dir);
        Kernel::SetApp0Directory(game_dir);
    }

    // 2. Load the main ELF/SELF module
    {
        const std::string stage = "Loading module: " +
            std::filesystem::path(target_path).filename().string();
        GPU::SetBootStatus(stage.c_str());
    }
    Loader::LoadedModule main_module;
    if (!Kernel::LoadModule(target_path, main_module)) {
        LOG_ERROR(General, "Failed to load target module: %s", target_path.c_str());
        summary.status = "fail";
        summary.stage  = "load";
        auto t1 = std::chrono::steady_clock::now();
        summary.duration_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        LuaInit::SubsystemRegistry::Instance().TeardownAll();

        PersistSummary(summary, report_path, regression_report_path);
        return -1;
    }

    // Hand the HLE guest-unwinder the main module's .eh_frame_hdr location
    // (needed for __cxa_throw / guest C++ exception support).
    HLE::SetGuestEhFrameHdr(main_module.eh_frame_hdr_addr, main_module.eh_frame_hdr_size);

    // 3. Execute the guest application on a dedicated worker thread.  The
    //    GLFW window was created on this (main) thread during subsystem init,
    //    so all GLFW event processing must stay here: the main thread runs the
    //    window/message loop at a steady cadence while the guest runs.
    {
        const std::string stage = "Module loaded: " + main_module.name;
        GPU::SetBootStatus(stage.c_str());
    }
    LOG_INFO(General, "Starting guest execution loop...");
    GPU::SetBootStatus("Starting guest execution");
    std::atomic<bool> guest_done{false};
    bool run_success = false;
    u32 guest_exit_code = 0;
    std::thread guest_thread([&]() {
        run_success = Kernel::Execute(main_module, &guest_exit_code);
        guest_done.store(true, std::memory_order_release);
    });

    while (!guest_done.load(std::memory_order_acquire)) {
        GPU::PumpWindowEvents();
        GPU::PollEvents();
        if (GPU::HasWindow() && GPU::ShouldCloseWindow()) {
            // The guest observes the stop flag on its next HLE dispatch and
            // unwinds back into Kernel::Execute.
            HLE::RequestStop();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 Hz
    }
    guest_thread.join();

    auto t1 = std::chrono::steady_clock::now();

    // Guest finished: keep the final frame visible until the user closes the
    // window (replaces the old GPU-side idle spin that ran on guest threads).
    if (GPU::HasWindow()) {
        GPU::RunIdleLoop();
    }
    summary.duration_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (run_success) {
        LOG_INFO(General, "Guest application completed execution successfully.");
        summary.status = "pass";
        summary.stage  = "execute";
    } else {
        LOG_ERROR(General, "Guest application execution crashed or failed.");
        summary.status = "fail";
        summary.stage  = "execute";
    }

    // Populate the import report and unresolved counters
    summary.top_imports         = HLE::GetImportReport();
    summary.resolved_imports    = summary.top_imports.size();
    summary.unresolved_imports  = HLE::GetUnresolvedImportCount();

    // 4. Shutdown Subsystems — teardown in reverse init order via the registry.
    LuaInit::SubsystemRegistry::Instance().TeardownAll();

    PersistSummary(summary, report_path, regression_report_path);

    // In strict-import mode, treat any unresolved import as a hard failure.
    if (strict_imports && summary.unresolved_imports > 0) {
        LOG_ERROR(General, "Strict-import mode: %llu unresolved import(s) detected.",
                  (unsigned long long)summary.unresolved_imports);
        return 3;
    }

    LOG_INFO(General, "pcsx5 shutdown cleanly.");
    return run_success ? static_cast<int>(guest_exit_code) : -1;
}
