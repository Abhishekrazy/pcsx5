#include "common/log.h"
#include "common/types.h"
#include "config/config.h"
#include "memory/memory.h"
#include "kernel/kernel.h"
#include "hle/hle.h"
#include "gpu/gpu.h"
#include "diagnostics/diagnostics.h"
#include "reports/reports.h"
#include <iostream>
#include <string>
#include <fstream>
#include <chrono>
#include <vector>
#include <cstdio>

namespace {

void PrintUsage() {
    std::printf("Usage:\n");
    std::printf("  pcsx5.exe [--strict-imports] [--report=<path>] [--regression-report=<path>]\n");
    std::printf("          [--log-file=<path>] [--crash-dir=<path>]\n");
    std::printf("          [--config-dir=<path>] [--title-id=<id>] <path_to_eboot.bin_or_elf>\n");
    std::printf("\nOptions:\n");
    std::printf("  --strict-imports             Fail (return non-zero) on unresolved imports.\n");
    std::printf("  --report=<path>              Write a JSON compatibility summary to <path>.\n");
    std::printf("  --regression-report=<path>   Write an aggregated markdown regression report.\n");
    std::printf("  --log-file=<path>            Mirror log output to <path>.\n");
    std::printf("  --crash-dir=<path>           Directory for crash-report bundles (default: pcsx5_crash).\n");
    std::printf("  --config-dir=<path>          Directory holding global.json + per-title overrides.\n");
    std::printf("  --title-id=<id>              PS5 title id (CUSAxxxxx) for per-title overrides and history.\n");
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

} // namespace

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
    bool strict_imports = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--strict-imports") {
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

    if (target_path.empty()) {
        PrintUsage();
        return 1;
    }

    // Bring up the configuration service as the very first subsystem so every
    // other initialiser (logging, diagnostics, HLE) can read its settings.
    if (config_dir.empty()) config_dir = "pcsx5_config";
    ConfigService::Initialize(config_dir);

    // Apply the effective (global + per-title) configuration to the runtime.
    const ConfigService::Config& cfg = ConfigService::EffectiveFor(title_id);
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

    // Install the crash-report handler early so any later crash (including
    // during subsystem init) is captured.
    Diagnostics::InstallCrashHandler(crash_dir);

    if (!title_id.empty()) {
        LOG_INFO(General, "Active title: %s", title_id.c_str());
    }

    // 1. Initialize Subsystems
    if (!Memory::Initialize()) {
        std::printf("FATAL: Failed to initialize Memory subsystem.\n");
        return -1;
    }

    if (!HLE::Initialize()) {
        std::printf("FATAL: Failed to initialize HLE subsystem.\n");
        Memory::Shutdown();
        return -1;
    }
    HLE::SetStrictImportMode(strict_imports);
    HLE::ResetRunStatistics();

    if (!Kernel::Initialize()) {
        std::printf("FATAL: Failed to initialize Kernel subsystem.\n");
        HLE::Shutdown();
        Memory::Shutdown();
        return -1;
    }

    if (!GPU::Initialize()) {
        std::printf("FATAL: Failed to initialize GPU subsystem.\n");
        Kernel::Shutdown();
        HLE::Shutdown();
        Memory::Shutdown();
        return -1;
    }

    LOG_INFO(General, "All subsystems initialized successfully.");

    Reports::CompatSummary summary = BuildSummary(target_path, title_id,
                                                 "fail", "load", 0.0);

    auto t0 = std::chrono::steady_clock::now();

    // 2. Load the main ELF/SELF module
    Loader::LoadedModule main_module;
    if (!Kernel::LoadModule(target_path, main_module)) {
        LOG_ERROR(General, "Failed to load target module: %s", target_path.c_str());
        summary.status = "fail";
        summary.stage  = "load";
        auto t1 = std::chrono::steady_clock::now();
        summary.duration_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        GPU::Shutdown();
        Kernel::Shutdown();
        HLE::Shutdown();
        Memory::Shutdown();

        // Persist the per-run summary in the form requested by the caller.
        if (!report_path.empty()) {
            std::string err;
            if (!Reports::WriteCompatSummary(report_path, summary, &err)) {
                LOG_WARN(General, "Failed to write compat summary: %s", err.c_str());
            }
        }
        Reports::AppendCompatHistory(ConfigService::Directory(), summary, nullptr);
        return -1;
    }

    // 3. Execute the guest application
    LOG_INFO(General, "Starting guest execution loop...");
    bool run_success = Kernel::Execute(main_module);

    auto t1 = std::chrono::steady_clock::now();
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

    // 4. Shutdown Subsystems
    GPU::Shutdown();
    Kernel::Shutdown();
    HLE::Shutdown();
    Memory::Shutdown();

    // One-shot compat summary (CLI opt-in).
    if (!report_path.empty()) {
        std::string err;
        if (!Reports::WriteCompatSummary(report_path, summary, &err)) {
            LOG_WARN(General, "Failed to write compat summary: %s", err.c_str());
        }
    }

    // Always append to the per-title jsonl history so the regression report
    // can spot drift over time.  Skip when the summary has no identifying
    // information (e.g. empty target path).
    if (!summary.target.empty()) {
        Reports::AppendCompatHistory(ConfigService::Directory(), summary, nullptr);
    }

    // Optional aggregated markdown regression report.
    if (!regression_report_path.empty()) {
        // Build a single-entry list (this run).  A multi-title report is
        // produced by orchestrators that loop over many titles.
        std::vector<Reports::RegressionEntry> entries;
        std::vector<Reports::CompatSummary> history =
            Reports::LoadCompatHistory(ConfigService::Directory(), summary.title_id, 32);
        // Drop the entry we just appended (it would otherwise be part of the
        // baseline against itself, masking the change we want to see).
        if (!history.empty() && history.front().timestamp_iso == summary.timestamp_iso) {
            history.erase(history.begin());
        }
        entries.push_back(Reports::EvaluateRegression(history, summary));
        std::string err;
        if (!Reports::WriteRegressionMarkdown(regression_report_path, entries, &err)) {
            LOG_WARN(General, "Failed to write regression report: %s", err.c_str());
        }
    }

    // In strict-import mode, treat any unresolved import as a hard failure.
    if (strict_imports && summary.unresolved_imports > 0) {
        LOG_ERROR(General, "Strict-import mode: %llu unresolved import(s) detected.",
                  (unsigned long long)summary.unresolved_imports);
        return 3;
    }

    LOG_INFO(General, "pcsx5 shutdown cleanly.");
    return run_success ? 0 : -1;
}
