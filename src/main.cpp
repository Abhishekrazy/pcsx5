#include "common/log.h"
#include "common/types.h"
#include "memory/memory.h"
#include "kernel/kernel.h"
#include "hle/hle.h"
#include "gpu/gpu.h"
#include <iostream>
#include <string>
#include <fstream>
#include <chrono>
#include <vector>
#include <cstdio>

namespace {

struct CompatReport {
    std::string target;
    std::string status;          // "pass" | "fail" | "error"
    std::string stage;           // "load" | "execute"
    double       duration_ms = 0;
    u64          resolved_imports = 0;
    u64          unresolved_imports = 0;
    std::vector<HLE::ImportStats> imports;
};

void WriteCompatReport(const std::string& path, const CompatReport& report) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        std::printf("Failed to open compatibility report: %s\n", path.c_str());
        return;
    }
    out << "{\n";
    out << "  \"target\": \"" << report.target << "\",\n";
    out << "  \"status\": \"" << report.status << "\",\n";
    out << "  \"stage\":  \"" << report.stage << "\",\n";
    out << "  \"duration_ms\": " << report.duration_ms << ",\n";
    out << "  \"resolved_imports\": " << report.resolved_imports << ",\n";
    out << "  \"unresolved_imports\": " << report.unresolved_imports << ",\n";
    out << "  \"imports\": [\n";
    for (size_t i = 0; i < report.imports.size(); ++i) {
        const auto& s = report.imports[i];
        out << "    { \"module\": \"" << s.module_name
            << "\", \"nid\": \"" << s.name
            << "\", \"calls\": " << s.call_count
            << ", \"thunk\": \"0x" << std::hex << s.thunk_address << std::dec
            << "\", \"last_caller\": \"0x" << std::hex << s.last_caller_rip << std::dec << "\" }";
        if (i + 1 < report.imports.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

void PrintUsage() {
    std::printf("Usage:\n");
    std::printf("  pcsx5.exe [--strict-imports] [--report=<path>] <path_to_eboot.bin_or_elf>\n");
    std::printf("\nOptions:\n");
    std::printf("  --strict-imports    Fail (return non-zero) if the guest requests an unresolved import.\n");
    std::printf("  --report=<path>     Write a JSON compatibility report to <path>.\n");
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
    bool strict_imports = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--strict-imports") {
            strict_imports = true;
        } else if (a.rfind("--report=", 0) == 0) {
            report_path = a.substr(9);
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

    CompatReport report;
    report.target = target_path;

    auto t0 = std::chrono::steady_clock::now();

    // 2. Load the main ELF/SELF module
    Loader::LoadedModule main_module;
    if (!Kernel::LoadModule(target_path, main_module)) {
        LOG_ERROR(General, "Failed to load target module: %s", target_path.c_str());
        report.status = "fail";
        report.stage  = "load";

        GPU::Shutdown();
        Kernel::Shutdown();
        HLE::Shutdown();
        Memory::Shutdown();
        if (!report_path.empty()) WriteCompatReport(report_path, report);
        return -1;
    }

    // 3. Execute the guest application
    LOG_INFO(General, "Starting guest execution loop...");
    bool run_success = Kernel::Execute(main_module);

    auto t1 = std::chrono::steady_clock::now();
    report.duration_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (run_success) {
        LOG_INFO(General, "Guest application completed execution successfully.");
        report.status = "pass";
        report.stage  = "execute";
    } else {
        LOG_ERROR(General, "Guest application execution crashed or failed.");
        report.status = "fail";
        report.stage  = "execute";
    }

    // Populate the import report and unresolved counters
    report.imports           = HLE::GetImportReport();
    report.resolved_imports  = report.imports.size();
    report.unresolved_imports = HLE::GetUnresolvedImportCount();

    // 4. Shutdown Subsystems
    GPU::Shutdown();
    Kernel::Shutdown();
    HLE::Shutdown();
    Memory::Shutdown();

    if (!report_path.empty()) WriteCompatReport(report_path, report);

    // In strict-import mode, treat any unresolved import as a hard failure.
    if (strict_imports && report.unresolved_imports > 0) {
        LOG_ERROR(General, "Strict-import mode: %llu unresolved import(s) detected.",
                  (unsigned long long)report.unresolved_imports);
        return 3;
    }

    LOG_INFO(General, "pcsx5 shutdown cleanly.");
    return run_success ? 0 : -1;
}
