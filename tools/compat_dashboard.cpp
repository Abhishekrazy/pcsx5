// I2.4: Compat report dashboard — aggregates bot-run report outputs into a
// markdown table showing which stage each title reaches per run.
//
// Usage:
//   compat_dashboard <bot_log_dir> [--output=<path>] [--json]
//   compat_dashboard --scan <replay_manifest> [--output=<path>]
//
// Scans the bot-run crash bundle directories (<bot_log_dir>/YYYYMMDD_HHMMSS_*/)
// for compat_report.json files and assembles a markdown dashboard.
//
// Output: COMPAT_DASHBOARD.md (or specified path) with a table:
//   | Title ID | Date | Status | Duration | Stage | Git Rev | Log |
//   |----------|------|--------|----------|-------|---------|-----|

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

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Simple JSON field extraction (no dependency on nlohmann in standalone tool).
// ---------------------------------------------------------------------------
std::string ExtractJsonString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";

    // Find the value after ':'
    auto colon = json.find(':', pos + search.size());
    if (colon == std::string::npos) return "";

    // Skip whitespace.
    auto val_start = json.find_first_not_of(" \t\r\n", colon + 1);
    if (val_start == std::string::npos) return "";

    if (json[val_start] == '"') {
        // String value: find closing quote.
        auto val_end = json.find('"', val_start + 1);
        if (val_end == std::string::npos) return "";
        return json.substr(val_start + 1, val_end - val_start - 1);
    } else if (json[val_start] == '{' || json[val_start] == '[') {
        // Skip complex value (not supported).
        return "";
    } else {
        // Number or literal (true/false/null).
        auto val_end = json.find_first_of(",}\n\r", val_start);
        if (val_end == std::string::npos) return json.substr(val_start);
        return json.substr(val_start, val_end - val_start);
    }
}

double ExtractJsonNumber(const std::string& json, const std::string& key) {
    std::string s = ExtractJsonString(json, key);
    if (s.empty()) return 0.0;
    return std::atof(s.c_str());
}

bool ExtractJsonBool(const std::string& json, const std::string& key) {
    std::string s = ExtractJsonString(json, key);
    return s == "true";
}

// ---------------------------------------------------------------------------
// Run record parsed from a bot-run bundle.
// ---------------------------------------------------------------------------
struct RunRecord {
    std::string title_id;
    std::string date_str;
    std::string status;     // "boot", "menu", "gameplay", "crash", "hang", "clean"
    std::string stage;      // last boot stage
    double      duration_ms = 0.0;
    std::string git_revision;
    std::string log_path;
    bool        crash      = false;
    bool        has_report = false;
};

// ---------------------------------------------------------------------------
// Parse a compat_report.json file.
// ---------------------------------------------------------------------------
RunRecord ParseReport(const fs::path& report_path) {
    RunRecord rec;
    rec.has_report = true;

    std::ifstream f(report_path);
    if (!f) return rec;

    std::stringstream ss;
    ss << f.rdbuf();
    std::string json = ss.str();

    rec.title_id     = ExtractJsonString(json, "title_id");
    rec.status       = ExtractJsonString(json, "status");
    rec.stage        = ExtractJsonString(json, "stage");
    rec.duration_ms  = ExtractJsonNumber(json, "duration_ms");
    rec.git_revision = ExtractJsonString(json, "git_revision");

    // Auto-detect crash status from log patterns.
    rec.crash = (rec.status == "crash" || rec.status == "error");
    if (rec.status.empty()) {
        // Check for crash hints in the filename.
        rec.status = "unknown";
    }

    return rec;
}

// ---------------------------------------------------------------------------
// Scan a bot-run crash bundle directory for reports.
// ---------------------------------------------------------------------------
std::vector<RunRecord> ScanBundleDir(const fs::path& root_dir) {
    std::vector<RunRecord> records;

    if (!fs::exists(root_dir)) return records;

    for (const auto& entry : fs::directory_iterator(root_dir)) {
        if (!entry.is_directory()) continue;

        fs::path report_file = entry.path() / "compat_report.json";
        fs::path log_file = entry.path() / "bot_log.txt";

        if (!fs::exists(report_file)) continue;

        RunRecord rec = ParseReport(report_file);

        // Extract date from directory name (YYYYMMDD_HHMMSS_<title>).
        std::string dir_name = entry.path().filename().string();
        if (dir_name.size() >= 15) {
            // Format: YYYYMMDD_HHMMSS_title
            rec.date_str = dir_name.substr(0, 4) + "-" +
                           dir_name.substr(4, 2) + "-" +
                           dir_name.substr(6, 2) + " " +
                           dir_name.substr(9, 2) + ":" +
                           dir_name.substr(11, 2) + ":" +
                           dir_name.substr(13, 2);

            if (rec.title_id.empty() && dir_name.size() > 16) {
                rec.title_id = dir_name.substr(16);
            }
        }

        // Check for hang indicator in log.
        if (fs::exists(log_file)) {
            rec.log_path = log_file.string();

            std::ifstream lf(log_file);
            std::string line;
            while (std::getline(lf, line)) {
                if (line.find("HANG") != std::string::npos ||
                    line.find("no flip within") != std::string::npos) {
                    if (rec.status == "crash" || rec.status == "unknown") {
                        rec.status = "hang";
                    }
                }
            }
        }

        // Fix duration if report doesn't have it.
        if (rec.duration_ms <= 0.0 && fs::exists(log_file)) {
            rec.duration_ms = static_cast<double>(fs::file_size(log_file)) / 100.0; // rough estimate
        }

        records.push_back(rec);
    }

    // Sort by date (directory name string-sorted = chronological).
    std::sort(records.begin(), records.end(),
              [](const RunRecord& a, const RunRecord& b) {
                  return a.date_str < b.date_str;
              });

    return records;
}

// ---------------------------------------------------------------------------
// Format duration as human-readable.
// ---------------------------------------------------------------------------
std::string FormatDuration(double ms) {
    if (ms < 1000) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f ms", ms);
        return buf;
    }
    double sec = ms / 1000.0;
    if (sec < 120) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f s", sec);
        return buf;
    }
    double min = sec / 60.0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f min", min);
    return buf;
}

// ---------------------------------------------------------------------------
// Generate markdown dashboard table.
// ---------------------------------------------------------------------------
std::string GenerateDashboard(const std::vector<RunRecord>& records,
                               const std::string& source) {
    std::ostringstream os;

    time_t now = std::time(nullptr);
    char time_buf[32];
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S UTC",
                  std::gmtime(&now));

    os << "# Compatibility Report Dashboard\n\n";
    os << "Generated: " << time_buf << "  \n";
    os << "Source: " << source << "  \n";
    os << "Total runs: " << records.size() << "\n\n";

    // Summary counts.
    int boot = 0, menu = 0, gameplay = 0, crash = 0, hang = 0, clean = 0, unknown = 0;
    for (const auto& r : records) {
        if (r.status == "boot")       boot++;
        else if (r.status == "menu")  menu++;
        else if (r.status == "gameplay" || r.status == "playable") gameplay++;
        else if (r.status == "crash" || r.status == "error") crash++;
        else if (r.status == "hang")  hang++;
        else if (r.status == "clean" || r.status == "exit") clean++;
        else unknown++;
    }

    os << "## Summary\n\n";
    os << "| Stage | Count |\n";
    os << "|-------|-------|\n";
    if (boot > 0)     os << "| Boot | " << boot << " |\n";
    if (menu > 0)     os << "| Menu | " << menu << " |\n";
    if (gameplay > 0) os << "| Gameplay | " << gameplay << " |\n";
    if (clean > 0)    os << "| Clean Exit | " << clean << " |\n";
    if (crash > 0)    os << "| Crash | " << crash << " |\n";
    if (hang > 0)     os << "| Hang | " << hang << " |\n";
    if (unknown > 0)  os << "| Unknown | " << unknown << " |\n";
    os << "\n";

    // Detail table.
    os << "## Per-Run Detail\n\n";
    os << "| # | Title ID | Date | Status | Duration | Last Stage |\n";
    os << "|---|----------|------|--------|----------|------------|\n";

    for (size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        os << "| " << (i + 1)
           << " | " << (r.title_id.empty() ? "?" : r.title_id)
           << " | " << (r.date_str.empty() ? "?" : r.date_str)
           << " | " << r.status
           << " | " << FormatDuration(r.duration_ms)
           << " | " << (r.stage.empty() ? "?" : r.stage)
           << " |\n";
    }

    os << "\n---\n\n";
    os << "Generated by `compat_dashboard` | I2.4\n";

    return os.str();
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr,
            "Usage:\n"
            "  compat_dashboard <bundle_dir> [--output=<path>]\n"
            "  compat_dashboard --scan <replay_manifest> [--output=<path>]\n"
            "\n"
            "Scans <bundle_dir> (default: pcsx5_crash/) for bot-run crash\n"
            "bundles and their compat_report.json files, then generates a\n"
            "markdown compatibility dashboard.\n");
        return 2;
    }

    std::string bundle_dir = argv[1];
    std::string output_path = "COMPAT_DASHBOARD.md";

    // Skip --scan flag (not yet implemented - scan is the default mode).
    if (bundle_dir == "--scan" || bundle_dir == "-s") {
        if (argc < 3) {
            std::fprintf(stderr, "Usage: --scan <replay_manifest_or_bundle_dir>\n");
            return 2;
        }
        bundle_dir = argv[2];
    }

    // Check for --output flag (any position).
    for (int i = 2; i < argc; ++i) {
        if (std::strncmp(argv[i], "--output=", 9) == 0) {
            output_path = argv[i] + 9;
        }
    }

    // Determine the directory to scan.
    fs::path scan_path = bundle_dir;
    if (!fs::exists(scan_path)) {
        scan_path = "pcsx5_crash";
        if (!fs::exists(scan_path)) {
            std::fprintf(stderr, "ERROR: bundle directory not found: '%s' or 'pcsx5_crash/'\n",
                         bundle_dir.c_str());
            return 2;
        }
    }

    std::vector<RunRecord> records = ScanBundleDir(scan_path);

    if (records.empty()) {
        std::fprintf(stdout, "No crash bundle reports found in '%s'.\n",
                     scan_path.string().c_str());
        // Create empty dashboard.
        std::string dashboard = "# Compatibility Report Dashboard\n\n"
                                "No runs recorded yet.\n";
        std::ofstream out(output_path);
        if (out) {
            out << dashboard;
            out.close();
            std::fprintf(stdout, "Dashboard written: %s\n", output_path.c_str());
        }
        return 0;
    }

    std::string dashboard = GenerateDashboard(records, scan_path.string());

    std::ofstream out(output_path);
    if (!out) {
        std::fprintf(stderr, "ERROR: cannot write output: %s\n", output_path.c_str());
        return 2;
    }
    out << dashboard;
    out.close();

    std::fprintf(stdout, "Dashboard written: %s (%zu runs)\n",
                 output_path.c_str(), records.size());
    return 0;
}
