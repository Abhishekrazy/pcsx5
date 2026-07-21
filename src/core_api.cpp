#include "core_api.h"

#include "common/log.h"
#include "common/nid.h"
#include "common/types.h"
#include "loader/pkg.h"
#include "config/config.h"
#include "memory/memory.h"
#include "kernel/kernel.h"
#include "hle/hle.h"
#include "hle/keystone.h"
#include "gpu/gpu.h"
#include "diagnostics/diagnostics.h"
#include "reports/reports.h"
#include "lua/lua_init.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

// ---------------------------------------------------------------------------
// Session state.  The core globals are one-shot: a single game session is
// live between pcsx5_init() and pcsx5_shutdown().
// ---------------------------------------------------------------------------
struct CoreState {
    // Options (copied; the caller's pcsx5_options may go out of scope)
    std::string config_dir;
    std::string crash_dir;
    std::string log_file;
    std::string title_id;
    std::string report_path;
    std::string regression_report_path;
    bool strict_imports = false;
    bool in_proc = false;

    // Run state
    Loader::LoadedModule main_module;
    Reports::CompatSummary summary;
    std::chrono::steady_clock::time_point t0;
    bool run_success = false;
    u32 guest_exit_code = 0;
    int  run_result = -1;

    // Window-handle reporting (in-proc mode)
    pcsx5_window_cb window_cb = nullptr;
    void* window_user = nullptr;
    unsigned long long hwnd = 0;
    bool hwnd_reported = false;

    bool initialized = false;
    bool loaded = false;
};

CoreState g_state;

void ReportWindowHandle(unsigned long long hwnd, void* /*user*/) {
    g_state.hwnd = hwnd;
    if (g_state.window_cb && !g_state.hwnd_reported) {
        g_state.hwnd_reported = true;
        g_state.window_cb(hwnd, g_state.window_user);
    }
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
// pcsx5_init — configuration, logging, crash handler, NID database, and the
// Lua-driven subsystem init chain.
// ---------------------------------------------------------------------------
PCSX5_API int pcsx5_init(const pcsx5_options* options, pcsx5_log_cb log_cb, void* log_user) {
    g_state = CoreState{};

    if (options) {
        if (options->config_dir)             g_state.config_dir = options->config_dir;
        if (options->crash_dir)              g_state.crash_dir = options->crash_dir;
        if (options->log_file)               g_state.log_file = options->log_file;
        if (options->title_id)               g_state.title_id = options->title_id;
        if (options->report_path)            g_state.report_path = options->report_path;
        if (options->regression_report_path) g_state.regression_report_path = options->regression_report_path;
        g_state.strict_imports = options->strict_imports != 0;
        g_state.in_proc        = options->in_proc != 0;

        if (options->embed) {
            // Embedded rendering: the GPU window starts hidden; the host
            // reparents it into its own window using the reported HWND.
            GPU::SetEmbeddedMode(true);
        }
        if (g_state.in_proc) {
            // Process-wide host hooks belong to the host process.
            Kernel::SetInProcMode(true);
            // Deliver the presentation HWND via callback, not stdout.
            GPU::SetWindowCreatedCallback(&ReportWindowHandle, nullptr);
        }
    }

    if (log_cb) {
        LogConfig::SetLogCallback(log_cb, log_user);
    }

    // Bring up the configuration service as the very first subsystem so every
    // other initialiser (logging, diagnostics, HLE) can read its settings.
    if (g_state.config_dir.empty()) g_state.config_dir = "pcsx5_config";
    ConfigService::Initialize(g_state.config_dir);
    GPU::SetBootStatus("Configuration loaded");

    // Apply the effective (global + per-title) configuration to the runtime.
    const ConfigService::Config& cfg = ConfigService::EffectiveFor(g_state.title_id);
    if (!g_state.title_id.empty() && ConfigService::ForTitle(g_state.title_id) != nullptr) {
        LOG_INFO(General, "Per-title config overrides active for %s (%s/titles/%s.json): "
                 "log level %s, strict imports %s",
                 g_state.title_id.c_str(), ConfigService::Directory().c_str(),
                 g_state.title_id.c_str(),
                 cfg.logging.min_level == LogLevel::Debug ? "Debug" :
                 cfg.logging.min_level == LogLevel::Info  ? "Info"  :
                 cfg.logging.min_level == LogLevel::Warn  ? "Warn"  : "Error",
                 cfg.hle.strict_imports ? "on" : "off");
    }
    if (!cfg.logging.file_path.empty()) {
        LogConfig::SetFileOutput(cfg.logging.file_path, cfg.logging.file_append);
    } else if (!g_state.log_file.empty()) {
        LogConfig::SetFileOutput(g_state.log_file, /*append=*/false);
    }
    if (cfg.logging.json_output) LogConfig::SetJsonOutput(true);
    for (int i = 0; i < 6; ++i) {
        LogConfig::SetLevel(static_cast<LogCategory>(i), cfg.logging.min_level);
    }
    if (!cfg.crash.bundle_dir.empty()) g_state.crash_dir = cfg.crash.bundle_dir;
    if (g_state.crash_dir.empty())     g_state.crash_dir = "pcsx5_crash";
    g_state.strict_imports = g_state.strict_imports || cfg.hle.strict_imports;

    // Hand the audio output settings to libSceAudioOut (0 = Off / silent-paced).
    HLE::SetAudioOutConfig(cfg.audio.backend, cfg.audio.volume);

    // Install the crash-report handler early so any later crash (including
    // during subsystem init) is captured.  Skipped in in-proc mode: the host
    // process owns its unhandled-exception policy.
    if (!g_state.in_proc) {
        Diagnostics::InstallCrashHandler(g_state.crash_dir);
    }

    // Load the external NID name database (assets/nid_db.txt) if present.
    // Look next to the (host) executable first, then in the CWD; missing file
    // is fine — the built-in table is always available.
    {
        std::vector<std::filesystem::path> nid_candidates;
        {
            char module_path[MAX_PATH] = {};
            const DWORD len = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
            if (len > 0 && len < MAX_PATH) {
                nid_candidates.push_back(
                    std::filesystem::path(module_path).parent_path() / "assets" / "nid_db.txt");
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

    if (!g_state.title_id.empty()) {
        LOG_INFO(General, "Active title: %s", g_state.title_id.c_str());
    }

    // Initialize Subsystems — driven by the Lua subsystem registry.
    // Falls back to the C++ default chain when Lua is unavailable.
    std::string init_error;
    if (!LuaInit::RunDefaultInit(&init_error)) {
        LOG_CRITICAL(General, "FATAL: Failed to initialize subsystems: %s", init_error.c_str());
        return -1;
    }

    // Post-init configuration that requires the HLE subsystem to be ready.
    HLE::SetStrictImportMode(g_state.strict_imports);
    HLE::ResetRunStatistics();
    HLE::SetSaveDataTitleId(g_state.title_id);
    // Route guest /savedata0 file I/O to the same host dir the save-data HLE
    // uses (creates the dir if needed).
    Kernel::SetSaveDataDirectory(HLE::GetSaveDataDir());

    LOG_INFO(General, "All subsystems initialized successfully.");
    g_state.initialized = true;
    return 0;
}

// ---------------------------------------------------------------------------
// pcsx5_load — module resolver, keystone ticket, main module load.
// ---------------------------------------------------------------------------
PCSX5_API int pcsx5_load(const char* eboot_path) {
    if (!g_state.initialized || !eboot_path || !*eboot_path) return -1;
    const std::string target_path = eboot_path;

    g_state.summary = BuildSummary(target_path, g_state.title_id, "fail", "load", 0.0);
    g_state.t0 = std::chrono::steady_clock::now();

    // Configure PRX module resolution: the game's own sce_module/ directory
    // first, then the user-supplied firmware modules directory from config.
    {
        const ConfigService::Config& cfg = ConfigService::EffectiveFor(g_state.title_id);
        const std::string game_dir =
            std::filesystem::path(target_path).parent_path().string();
        Kernel::ConfigureModuleResolver(game_dir, cfg.loader.firmware_modules_dir);
        Kernel::SetApp0Directory(game_dir);

        // Per-title keystone ticket: load <app0>/.keystone when present.
        // Absence is normal (not every title ships one) and not an error.
        const std::filesystem::path keystone_path =
            std::filesystem::path(game_dir) / ".keystone";
        std::error_code keystone_ec;
        if (std::filesystem::exists(keystone_path, keystone_ec)) {
            std::ifstream ks(keystone_path, std::ios::binary);
            std::vector<u8> blob((std::istreambuf_iterator<char>(ks)),
                                 std::istreambuf_iterator<char>());
            HLE::KeystoneHeader header;
            const HLE::KeystoneError err =
                HLE::ParseKeystoneHeader(blob.data(), blob.size(), &header);
            if (err == HLE::KeystoneError::kOk) {
                HLE::SetKeystoneBlob(std::move(blob));
                LOG_INFO(General, "KEYSTONE_LOADED version=%u.%02u type=%u size=%llu",
                         (header.version >> 24) & 0xFF, (header.version >> 16) & 0xFF,
                         header.type, header.file_size);
            } else {
                LOG_WARN(General, "KEYSTONE_INVALID reason=%s path=%s",
                         HLE::KeystoneErrorName(err), keystone_path.string().c_str());
            }
        }
    }

    // Load the main ELF/SELF module
    {
        const std::string stage = "Loading module: " +
            std::filesystem::path(target_path).filename().string();
        GPU::SetBootStatus(stage.c_str());
    }
    if (!Kernel::LoadModule(target_path, g_state.main_module)) {
        LOG_ERROR(General, "Failed to load target module: %s", target_path.c_str());
        g_state.summary.status = "fail";
        g_state.summary.stage  = "load";
        auto t1 = std::chrono::steady_clock::now();
        g_state.summary.duration_ms =
            std::chrono::duration<double, std::milli>(t1 - g_state.t0).count();
        return -1;
    }

    // Hand the HLE guest-unwinder the main module's .eh_frame_hdr location
    // (needed for __cxa_throw / guest C++ exception support).
    HLE::SetGuestEhFrameHdr(g_state.main_module.eh_frame_hdr_addr,
                            g_state.main_module.eh_frame_hdr_size);

    g_state.loaded = true;
    return 0;
}

// ---------------------------------------------------------------------------
// pcsx5_run — guest worker thread + window/message loop on the calling
// thread (GLFW was initialized here, so the pump must stay here).
// ---------------------------------------------------------------------------
PCSX5_API int pcsx5_run(pcsx5_window_cb window_cb, void* window_user) {
    if (!g_state.loaded) return -1;

    g_state.window_cb = window_cb;
    g_state.window_user = window_user;
    if (window_cb && g_state.hwnd != 0 && !g_state.hwnd_reported) {
        // The window already came up during pcsx5_init (subsystem init
        // creates it); deliver the stored handle now.
        g_state.hwnd_reported = true;
        window_cb(g_state.hwnd, window_user);
    }

    {
        const std::string stage = "Module loaded: " + g_state.main_module.name;
        GPU::SetBootStatus(stage.c_str());
    }
    LOG_INFO(General, "Starting guest execution loop...");
    GPU::SetBootStatus("Starting guest execution");
    std::atomic<bool> guest_done{false};
    std::thread guest_thread([&]() {
        g_state.run_success = Kernel::Execute(g_state.main_module, &g_state.guest_exit_code);
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

    // Guest finished: in standalone mode keep the final frame visible until
    // the user closes the window.  In in-proc mode the window is reparented
    // into the host UI, so just return — the host owns its lifetime.
    if (!g_state.in_proc && GPU::HasWindow()) {
        GPU::RunIdleLoop();
    }
    g_state.summary.duration_ms =
        std::chrono::duration<double, std::milli>(t1 - g_state.t0).count();

    if (g_state.run_success) {
        LOG_INFO(General, "Guest application completed execution successfully.");
        g_state.summary.status = "pass";
        g_state.summary.stage  = "execute";
    } else {
        LOG_ERROR(General, "Guest application execution crashed or failed.");
        g_state.summary.status = "fail";
        g_state.summary.stage  = "execute";
    }

    // Populate the import report and unresolved counters
    g_state.summary.top_imports         = HLE::GetImportReport();
    g_state.summary.resolved_imports    = g_state.summary.top_imports.size();
    g_state.summary.unresolved_imports  = HLE::GetUnresolvedImportCount();

    // In strict-import mode, treat any unresolved import as a hard failure.
    if (g_state.strict_imports && g_state.summary.unresolved_imports > 0) {
        LOG_ERROR(General, "Strict-import mode: %llu unresolved import(s) detected.",
                  (unsigned long long)g_state.summary.unresolved_imports);
        g_state.run_result = 3;
    } else {
        g_state.run_result =
            g_state.run_success ? static_cast<int>(g_state.guest_exit_code) : -1;
    }
    return g_state.run_result;
}

// ---------------------------------------------------------------------------
// pcsx5_stop — thread-safe stop request observed on the next HLE dispatch.
// ---------------------------------------------------------------------------
PCSX5_API void pcsx5_stop(void) {
    HLE::RequestStop();
}

// ---------------------------------------------------------------------------
// pcsx5_shutdown — teardown in reverse init order + persist the run summary.
// ---------------------------------------------------------------------------
PCSX5_API void pcsx5_shutdown(void) {
    if (!g_state.initialized) return;

    LuaInit::SubsystemRegistry::Instance().TeardownAll();

    if (g_state.loaded || !g_state.summary.target.empty()) {
        PersistSummary(g_state.summary, g_state.report_path,
                       g_state.regression_report_path);
    }

    LOG_INFO(General, "pcsx5 shutdown cleanly.");
    g_state.initialized = false;
    g_state.loaded = false;
}

// ---------------------------------------------------------------------------
// pcsx5_extract_pkg — extract every entry we can handle and print a short
// summary.  Returns 0 on success, non-zero on failure.
// ---------------------------------------------------------------------------
PCSX5_API int pcsx5_extract_pkg(const char* pkg_path, const char* out_dir) {
    if (!pkg_path || !out_dir) return 1;

    Loader::PkgImage image;
    if (!Loader::ParsePkg(pkg_path, image)) {
        std::printf("error: failed to parse PKG: %s\n", pkg_path);
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
                extracted, extracted == 1 ? "y" : "ies", skipped, out_dir);
    return extracted > 0 ? 0 : 1;
}
