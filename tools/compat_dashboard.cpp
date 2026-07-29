// I2.4: Compat report dashboard -- aggregates bot-run report outputs into a
// markdown table showing which stage each title reaches per run.
//
// Scans bot-run crash bundle directories (pcsx5_crash/YYYYMMDD_HHMMSS_*/)
// and/or individual --report JSON files produced by the emulator's
// --report=<path> flag (see reports.h / reports.cpp CompatSummary format).
//
// Usage:
//   compat_dashboard --dir <bundle_dir> [--output=<path>]
//   compat_dashboard --report <report.json> [--report <more.json> ...]
//   compat_dashboard <bundle_dir>  (shorthand for --dir)
//
// Output: COMPAT_DASHBOARD.md (or --output=<path>) with a summary count
// table and a per-run detail table.
//
// Status mapping from the CompatSummary format:
//   "pass" + stage="execute" -> gameplay
//   "pass" + stage="load"    -> boot
//   "fail" + stage="load"    -> crash (during load)
//   "fail" + stage="execute" -> crash
//   "error"                  -> crash
//   "hang" (inferred from log) -> hang

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <cctype>

namespace fs = std::filesystem;

namespace {

// ===========================================================================
// JSON extraction (minimal, no external dependency).  These functions parse
// just enough of the CompatSummary schema to extract the fields we need.
// ===========================================================================

// Skip whitespace and return the position of the first non-whitespace char.
const char* SkipWs(const char* p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        ++p;
    return p;
}

// Extract a JSON string value for a given key.  Returns empty string if
// the key is not found or the value is not a string.
std::string ExtractString(const std::string& json, const std::string& key) {
    // Search for  "key":
    const std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return {};

    pos += search.size();
    const char* p = json.data() + pos;
    p = SkipWs(p);
    if (*p != '"') return {}; // not a string value
    ++p; // skip opening quote

    std::string result;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1)) {
            ++p;
            switch (*p) {
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                case '"': result += '"';  break;
                case '\\':result += '\\'; break;
                default:  result += *p;   break;
            }
        } else {
            result += *p;
        }
        ++p;
    }
    return result;
}

// Extract a JSON number value for a given key.  Returns 0.0 if not found.
double ExtractNumber(const std::string& json, const std::string& key) {
    const std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0.0;

    pos += search.size();
    const char* p = json.data() + pos;
    p = SkipWs(p);
    if (*p == '"') {
        // Number stored as string (rare, but handle it).
        ++p;
        std::string num;
        while (*p && *p != '"') { num += *p; ++p; }
        return std::atof(num.c_str());
    }
    // Parse literal number.
    char* end = nullptr;
    double v = std::strtod(p, &end);
    (void)end;
    return v;
}

// ===========================================================================
// Data model
// ===========================================================================

// Status we assign to a run on the dashboard.
enum class RunStatus {
    Unknown,
    Boot,       // load succeeded but only reached boot stage
    Menu,       // title reached its menu screen
    Gameplay,   // passed menu, entered gameplay
    Crash,      // emulator/game crashed
    Hang,       // no progress (no flip, timeout)
};

const char* StatusLabel(RunStatus s) {
    switch (s) {
        case RunStatus::Boot:     return "boot";
        case RunStatus::Menu:     return "menu";
        case RunStatus::Gameplay: return "gameplay";
        case RunStatus::Crash:    return "crash";
        case RunStatus::Hang:     return "hang";
        default:                  return "unknown";
    }
}

struct RunRecord {
    std::string title_id;
    std::string date_str;       // human-readable date from dir name or timestamp_iso
    RunStatus   status = RunStatus::Unknown;
    std::string stage;          // last boot stage from report
    double      duration_ms = 0.0;
    std::string git_revision;
    std::string log_path;       // path to associated bot log if found
};

// ===========================================================================
// Parse a CompatSummary JSON (written by reports.cpp WriteCompatSummary)
// into a RunRecord.
// ===========================================================================
RunRecord ParseCompatReport(const std::string& json) {
    RunRecord rec;

    const std::string compat_status = ExtractString(json, "status");
    const std::string compat_stage  = ExtractString(json, "stage");

    rec.title_id    = ExtractString(json, "title_id");
    rec.stage       = compat_stage;
    rec.duration_ms = ExtractNumber(json, "duration_ms");
    rec.git_revision = ExtractString(json, "git_revision");
    rec.date_str    = ExtractString(json, "timestamp_iso");

    // Map (status, stage) from the compat format to dashboard status.
    if (compat_status == "pass") {
        if (compat_stage == "execute") {
            // Reached execution phase - either menu or gameplay.
            // Default to gameplay; caller can refine with heuristics.
            rec.status = RunStatus::Gameplay;
        } else if (compat_stage == "load") {
            rec.status = RunStatus::Boot;
        } else {
            rec.status = RunStatus::Boot; // partial progress
        }
    } else if (compat_status == "fail" || compat_status == "error") {
        rec.status = RunStatus::Crash;
    }

    return rec;
}

// ===========================================================================
// Read a report file from disk and parse it.
// ===========================================================================
RunRecord ReadReportFile(const fs::path& path) {
    std::ifstream f(path);
    if (!f) {
        RunRecord empty;
        return empty;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ParseCompatReport(ss.str());
}

// ===========================================================================
// Infer hang from log contents.
// ===========================================================================
bool LogIndicatesHang(const fs::path& log_path) {
    if (!fs::exists(log_path)) return false;
    std::ifstream lf(log_path);
    if (!lf) return false;
    std::string line;
    while (std::getline(lf, line)) {
        if (line.find("HANG") != std::string::npos ||
            line.find("no flip within") != std::string::npos ||
            line.find("TIMEOUT") != std::string::npos ||
            line.find("hung") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// ===========================================================================
// Scan a bot-run crash bundle directory (pcsx5_crash/YYYYMMDD_HHMMSS_*/)
// for report files and bot logs.
// ===========================================================================
std::vector<RunRecord> ScanBundleDir(const fs::path& root_dir) {
    std::vector<RunRecord> records;

    if (!fs::exists(root_dir) || !fs::is_directory(root_dir)) {
        return records;
    }

    for (const auto& entry : fs::directory_iterator(root_dir)) {
        if (!entry.is_directory()) continue;

        const fs::path dir_path = entry.path();
        const std::string dir_name = dir_path.filename().string();

        // Look for report JSON files in the directory.
        // We try several known filenames that various bots use.
        static const char* kReportNames[] = {
            "compat_report.json",
            "report.json",
            "compat.json",
        };

        fs::path report_path;
        for (const char* name : kReportNames) {
            fs::path candidate = dir_path / name;
            if (fs::exists(candidate)) {
                report_path = candidate;
                break;
            }
        }

        if (report_path.empty()) continue;

        RunRecord rec = ReadReportFile(report_path);

        // If title_id is empty, try to extract from directory name.
        if (rec.title_id.empty()) {
            // Directory format: YYYYMMDD_HHMMSS_titleid
            if (dir_name.size() > 16) {
                rec.title_id = dir_name.substr(16);
            }
        }

        // Extract date from directory name if not in report.
        if (rec.date_str.empty() && dir_name.size() >= 15) {
            // Format: YYYYMMDD_HHMMSS_...
            rec.date_str = dir_name.substr(0, 4) + "-" +
                           dir_name.substr(4, 2) + "-" +
                           dir_name.substr(6, 2) + " " +
                           dir_name.substr(9, 2) + ":" +
                           dir_name.substr(11, 2) + ":" +
                           dir_name.substr(13, 2);
        }

        // Look for bot log to check for hang.
        const fs::path log_file = dir_path / "bot_log.txt";
        if (fs::exists(log_file)) {
            rec.log_path = log_file.string();
            if (LogIndicatesHang(log_file)) {
                // Hang overrides crash / boot detection.
                rec.status = RunStatus::Hang;
            }
        }

        // If duration is missing or zero, estimate from log file size.
        if (rec.duration_ms <= 0.0 && !rec.log_path.empty()) {
            std::error_code ec;
            const auto fsize = fs::file_size(rec.log_path, ec);
            if (!ec && fsize > 0) {
                // Rough heuristic: ~1ms per 100 bytes of log.
                rec.duration_ms = static_cast<double>(fsize) / 100.0;
            }
        }

        records.push_back(std::move(rec));
    }

    // Sort by date (string-sorted = chronological for ISO format).
    std::sort(records.begin(), records.end(),
              [](const RunRecord& a, const RunRecord& b) {
                  return a.date_str < b.date_str;
              });
    return records;
}

// ===========================================================================
// Format duration as human-readable.
// ===========================================================================
std::string FormatDuration(double ms) {
    if (ms < 1000.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f ms", ms);
        return buf;
    }
    const double sec = ms / 1000.0;
    if (sec < 120.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f s", sec);
        return buf;
    }
    const double min = sec / 60.0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f min", min);
    return buf;
}

// ===========================================================================
// Generate markdown dashboard.
// ===========================================================================
std::string BuildDashboard(const std::vector<RunRecord>& records,
                           const std::string& source) {
    std::ostringstream os;

    // Timestamp.
    std::time_t now = std::time(nullptr);
    char time_buf[64];
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S UTC",
                  std::gmtime(&now));

    os << "# Compatibility Report Dashboard\n\n";
    os << "- Generated: " << time_buf << "\n";
    os << "- Source: " << source << "\n";
    os << "- Runs: " << records.size() << "\n\n";

    // Summary table.
    int boot_count = 0, menu_count = 0, gameplay_count = 0;
    int crash_count = 0, hang_count = 0, unknown_count = 0;

    for (const auto& r : records) {
        switch (r.status) {
            case RunStatus::Boot:     ++boot_count;     break;
            case RunStatus::Menu:     ++menu_count;     break;
            case RunStatus::Gameplay: ++gameplay_count;  break;
            case RunStatus::Crash:    ++crash_count;    break;
            case RunStatus::Hang:     ++hang_count;     break;
            default:                  ++unknown_count;  break;
        }
    }

    os << "## Summary\n\n";
    os << "| Stage | Count |\n";
    os << "|-------|------:|\n";
    if (gameplay_count > 0) os << "| Gameplay | " << gameplay_count << " |\n";
    if (menu_count > 0)     os << "| Menu     | " << menu_count     << " |\n";
    if (boot_count > 0)     os << "| Boot     | " << boot_count     << " |\n";
    if (crash_count > 0)    os << "| Crash    | " << crash_count    << " |\n";
    if (hang_count > 0)     os << "| Hang     | " << hang_count     << " |\n";
    if (unknown_count > 0)  os << "| Unknown  | " << unknown_count  << " |\n";
    if (gameplay_count + menu_count > 0) {
        os << "| **Playable** | **" << (gameplay_count + menu_count) << "** |\n";
    }
    os << "\n";

    // Per-run detail table.
    os << "## Per-Run Detail\n\n";
    os << "| # | Title ID | Date | Status | Duration | Last Stage |\n";
    os << "|---|----------|------|--------|----------|------------|\n";

    for (size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        os << "| " << (i + 1)
           << " | " << (r.title_id.empty() ? "?" : r.title_id)
           << " | " << (r.date_str.empty() ? "?" : r.date_str)
           << " | " << StatusLabel(r.status)
           << " | " << FormatDuration(r.duration_ms)
           << " | " << (r.stage.empty() ? "?" : r.stage)
           << " |\n";
    }

    os << "\n---\n\n";
    os << "Generated by `compat_dashboard` | I2.4\n";
    return os.str();
}

void PrintUsage() {
    std::fprintf(stderr,
        "Usage:\n"
        "  compat_dashboard --dir <bundle_dir> [--output=<path>]\n"
        "  compat_dashboard --report <report.json> [--report <more.json> ...]\n"
        "  compat_dashboard <bundle_dir>\n"
        "\n"
        "Scans bot-run crash bundle directories (pcsx5_crash/YYYYMMDD_HHMMSS_*/)\n"
        "for compat_report.json, or loads individual --report files produced by\n"
        "the emulator's --report=<path> flag.  Outputs a markdown table to\n"
        "COMPAT_DASHBOARD.md (or --output=<path>).\n"
        "\n"
        "Status mapping:\n"
        "  CompatSummary status=\"pass\", stage=\"execute\" -> gameplay\n"
        "  CompatSummary status=\"pass\", stage=\"load\"    -> boot\n"
        "  CompatSummary status=\"fail\" or \"error\"         -> crash\n"
        "  Log patterns (HANG, TIMEOUT, hung)               -> hang\n");
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    if (argc < 2) {
        PrintUsage();
        return 2;
    }

    std::string output_path = "COMPAT_DASHBOARD.md";
    std::vector<std::string> report_files;  // individual --report files
    std::string bundle_dir;                 // --dir target

    // Parse arguments.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            bundle_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--report") == 0 && i + 1 < argc) {
            report_files.push_back(argv[++i]);
        } else if (std::strncmp(argv[i], "--output=", 9) == 0) {
            output_path = argv[i] + 9;
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (argv[i][0] == '-') {
            // Unknown flag but might be positional if it starts with -
            std::fprintf(stderr, "WARNING: ignoring unknown flag: %s\n", argv[i]);
        } else {
            // Positional argument: treat as bundle_dir shorthand.
            if (bundle_dir.empty()) {
                bundle_dir = argv[i];
            } else {
                std::fprintf(stderr,
                    "WARNING: unexpected positional arg: %s\n", argv[i]);
            }
        }
    }

    // Collect records from all sources.
    std::vector<RunRecord> records;

    // 1. Scan bundle directory.
    if (!bundle_dir.empty()) {
        fs::path scan_path = bundle_dir;
        if (!fs::exists(scan_path)) {
            // Fall back to pcsx5_crash/ as the default scanning root.
            scan_path = "pcsx5_crash";
            if (fs::exists(scan_path)) {
                std::fprintf(stdout,
                    "Note: '%s' not found, scanning 'pcsx5_crash/' instead.\n",
                    bundle_dir.c_str());
            }
        }
        if (fs::exists(scan_path)) {
            auto dir_records = ScanBundleDir(scan_path);
            records.insert(records.end(),
                           std::make_move_iterator(dir_records.begin()),
                           std::make_move_iterator(dir_records.end()));
            std::fprintf(stdout, "Scanned %s: %zu run(s) found\n",
                         scan_path.string().c_str(), dir_records.size());
        } else {
            std::fprintf(stderr,
                "WARNING: bundle directory not found: '%s'\n",
                bundle_dir.c_str());
        }
    }

    // 2. Load individual --report files.
    for (const auto& rp : report_files) {
        RunRecord rec = ReadReportFile(rp);
        if (rec.title_id.empty() && rec.stage.empty()) {
            std::fprintf(stderr, "WARNING: could not parse report: %s\n",
                         rp.c_str());
            continue;
        }
        // Extract date from directory name if the report lacks timestamp.
        if (rec.date_str.empty()) {
            fs::path p(rp);
            std::string parent = p.parent_path().filename().string();
            if (parent.size() >= 15 &&
                parent[0] >= '0' && parent[0] <= '9') {
                rec.date_str = parent.substr(0, 4) + "-" +
                               parent.substr(4, 2) + "-" +
                               parent.substr(6, 2) + " " +
                               parent.substr(9, 2) + ":" +
                               parent.substr(11, 2) + ":" +
                               parent.substr(13, 2);
            }
        }
        records.push_back(std::move(rec));
    }

    if (records.empty()) {
        std::fprintf(stdout, "No reports found.\n");

        // Create empty dashboard.
        std::time_t now = std::time(nullptr);
        char time_buf[64];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S UTC",
                      std::gmtime(&now));

        std::string empty_dashboard =
            "# Compatibility Report Dashboard\n\n"
            "Generated: " + std::string(time_buf) + "\n\n"
            "No runs recorded yet.\n";

        std::ofstream out(output_path);
        if (out) {
            out << empty_dashboard;
            out.close();
            std::fprintf(stdout, "Dashboard written: %s (empty)\n",
                         output_path.c_str());
        }
        return 0;
    }

    // Build source description for the dashboard header.
    std::string source_desc;
    if (!bundle_dir.empty()) source_desc = "dir: " + bundle_dir;
    if (!report_files.empty()) {
        if (!source_desc.empty()) source_desc += " + ";
        source_desc += std::to_string(report_files.size()) +
                       " report file(s)";
    }

    const std::string dashboard = BuildDashboard(records, source_desc);

    std::ofstream out(output_path);
    if (!out) {
        std::fprintf(stderr, "ERROR: cannot write output: %s\n",
                     output_path.c_str());
        return 2;
    }
    out << dashboard;
    out.close();

    std::fprintf(stdout, "Dashboard written: %s (%zu runs)\n",
                 output_path.c_str(), records.size());
    return 0;
}
