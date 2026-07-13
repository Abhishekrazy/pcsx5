#include "reports.h"
#include "../common/log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

// ===========================================================================
// JSON helpers (local to this module; schema is small and stable)
// ===========================================================================
namespace {

void WriteString(std::ostream& os, const std::string& s) {
    os << '"';
    for (char c : s) {
        switch (c) {
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\b': os << "\\b";  break;
            case '\f': os << "\\f";  break;
            case '\n': os << "\\n";  break;
            case '\r': os << "\\r";  break;
            case '\t': os << "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    os << buf;
                } else {
                    os << c;
                }
        }
    }
    os << '"';
}

void WriteNumber(std::ostream& os, double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    os << buf;
}

std::string IsoNow() {
    // Same policy as diagnostics: UTC ISO-8601 with millisecond precision.
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02uZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, static_cast<unsigned>(tm.tm_sec));
    return buf;
}

std::string SanitiseTitle(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if ((c >= '0' && c <= '9') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            c == '-' || c == '_' || c == '.') {
            out += c;
        } else {
            out += '_';
        }
    }
    if (out.empty()) out = "untitled";
    return out;
}

void FillDefaults(Reports::CompatSummary& s) {
    if (s.timestamp_iso.empty()) s.timestamp_iso = IsoNow();
    if (s.schema_version.empty()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "pcsx5.compat.v%d",
                      Reports::kCurrentSchemaVersion);
        s.schema_version = buf;
    }
    if (s.title_id.empty()) s.title_id = SanitiseTitle(s.target);
}

} // namespace

// ===========================================================================
// Serialisation
// ===========================================================================
namespace Reports {

std::string SerializeSummaryJsonl(const CompatSummary& s) {
    CompatSummary copy = s;
    FillDefaults(copy);

    // Top imports are already in descending order from HLE::GetImportReport.
    // We still trim to kTopImports and pick the most-called ones.
    std::vector<HLE::ImportStats> top;
    if (copy.top_imports.size() > kTopImports) {
        top.assign(copy.top_imports.begin(),
                   copy.top_imports.begin() + static_cast<std::ptrdiff_t>(kTopImports));
    } else {
        top = copy.top_imports;
    }

    std::ostringstream os;
    os << "{\"schema_version\":\"" << copy.schema_version << "\","
       << "\"timestamp_iso\":\"" << copy.timestamp_iso << "\","
       << "\"title_id\":\"";        WriteString(os, copy.title_id);
    os << "\",\"target\":\"";       WriteString(os, copy.target);
    os << "\",\"git_revision\":\""; WriteString(os, copy.git_revision);
    os << "\",\"status\":\"";       WriteString(os, copy.status);
    os << "\",\"stage\":\"";        WriteString(os, copy.stage);
    os << "\",\"duration_ms\":";    WriteNumber(os, copy.duration_ms);
    os << ",\"resolved_imports\":" <<   copy.resolved_imports
       << ",\"unresolved_imports\":" << copy.unresolved_imports
       << ",\"top_imports\":[";
    for (std::size_t i = 0; i < top.size(); ++i) {
        if (i) os << ',';
        const auto& imp = top[i];
        os << "{\"module\":\"";     WriteString(os, imp.module_name);
        os << "\",\"nid\":\"";      WriteString(os, imp.name);
        os << "\",\"calls\":" << imp.call_count
           << ",\"thunk\":\"0x";    WriteNumber(os, static_cast<double>(imp.thunk_address));
        os << "\",\"last_caller\":\"0x"; WriteNumber(os, static_cast<double>(imp.last_caller_rip));
        os << "\"}";
    }
    os << "]}";
    return os.str();
}

// ===========================================================================
// File I/O
// ===========================================================================
bool WriteCompatSummary(const std::string& path, const CompatSummary& s,
                        std::string* error) {
    CompatSummary copy = s;
    FillDefaults(copy);

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::trunc | std::ios::binary);
    if (!out) {
        if (error) *error = "could not open summary file for writing";
        return false;
    }

    // Pretty-printed version of the same payload.
    std::vector<HLE::ImportStats> top;
    if (copy.top_imports.size() > kTopImports) {
        top.assign(copy.top_imports.begin(),
                   copy.top_imports.begin() + static_cast<std::ptrdiff_t>(kTopImports));
    } else {
        top = copy.top_imports;
    }

    out << "{\n";
    out << "  \"schema_version\": \"" << copy.schema_version << "\",\n";
    out << "  \"timestamp_iso\":  \"" << copy.timestamp_iso << "\",\n";
    out << "  \"title_id\":       \""; WriteString(out, copy.title_id);   out << "\",\n";
    out << "  \"target\":         \""; WriteString(out, copy.target);     out << "\",\n";
    out << "  \"git_revision\":   \""; WriteString(out, copy.git_revision); out << "\",\n";
    out << "  \"status\":         \"" << copy.status << "\",\n";
    out << "  \"stage\":          \"" << copy.stage  << "\",\n";
    out << "  \"duration_ms\":    ";   WriteNumber(out, copy.duration_ms); out << ",\n";
    out << "  \"resolved_imports\":   " << copy.resolved_imports   << ",\n";
    out << "  \"unresolved_imports\": " << copy.unresolved_imports << ",\n";
    out << "  \"top_imports\": [\n";
    for (std::size_t i = 0; i < top.size(); ++i) {
        const auto& imp = top[i];
        out << "    { \"module\": \"";     WriteString(out, imp.module_name);
        out << "\", \"nid\": \"";          WriteString(out, imp.name);
        out << "\", \"calls\": " << imp.call_count
            << ", \"thunk\": \"0x";        WriteNumber(out, static_cast<double>(imp.thunk_address));
        out << "\", \"last_caller\": \"0x";WriteNumber(out, static_cast<double>(imp.last_caller_rip));
        out << "\" }";
        if (i + 1 < top.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return out.good();
}

std::string AppendCompatHistory(const std::string& dir,
                                CompatSummary s,
                                std::string* error) {
    FillDefaults(s);
    if (s.title_id.empty()) s.title_id = SanitiseTitle(s.target);

    std::error_code ec;
    const std::string history_dir = dir + "/compat";
    std::filesystem::create_directories(history_dir, ec);
    if (ec) {
        if (error) *error = "could not create history dir";
        return {};
    }
    const std::string path = history_dir + "/" + s.title_id + ".jsonl";
    std::ofstream out(path, std::ios::app | std::ios::binary);
    if (!out) {
        if (error) *error = "could not open jsonl for append";
        return {};
    }
    out << SerializeSummaryJsonl(s) << "\n";
    out.flush();
    if (!out.good()) {
        if (error) *error = "write failed";
        return {};
    }
    return path;
}

namespace {

// Minimal line-delimited JSON loader: parses just the fields we wrote.  We
// keep this in a .cpp-local helper so the public header stays free of JSON
// dependencies.
bool ParseSummaryLine(const std::string& line, Reports::CompatSummary& out) {
    auto find_string = [&](const std::string& key) -> std::string {
        const std::string needle = "\"" + key + "\":\"";
        auto pos = line.find(needle);
        if (pos == std::string::npos) return {};
        pos += needle.size();
        std::string result;
        while (pos < line.size() && line[pos] != '"') {
            if (line[pos] == '\\' && pos + 1 < line.size()) {
                char c = line[pos + 1];
                switch (c) {
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    case '"': result += '"';  break;
                    case '\\':result += '\\'; break;
                    default:  result += c;
                }
                pos += 2;
            } else {
                result += line[pos++];
            }
        }
        return result;
    };
    auto find_u64 = [&](const std::string& key) -> u64 {
        const std::string needle = "\"" + key + "\":";
        auto pos = line.find(needle);
        if (pos == std::string::npos) return 0;
        pos += needle.size();
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
        u64 v = 0;
        bool any = false;
        while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
            v = v * 10 + (line[pos] - '0');
            ++pos;
            any = true;
        }
        (void)any;
        return v;
    };
    auto find_double = [&](const std::string& key) -> double {
        const std::string needle = "\"" + key + "\":";
        auto pos = line.find(needle);
        if (pos == std::string::npos) return 0.0;
        pos += needle.size();
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
        std::string num;
        while (pos < line.size() &&
               (line[pos] == '.' || line[pos] == '-' || line[pos] == '+' ||
                line[pos] == 'e' || line[pos] == 'E' ||
                (line[pos] >= '0' && line[pos] <= '9'))) {
            num += line[pos++];
        }
        if (num.empty()) return 0.0;
        return std::strtod(num.c_str(), nullptr);
    };

    out = Reports::CompatSummary{};
    out.schema_version     = find_string("schema_version");
    out.timestamp_iso      = find_string("timestamp_iso");
    out.title_id           = find_string("title_id");
    out.target             = find_string("target");
    out.git_revision       = find_string("git_revision");
    out.status             = find_string("status");
    out.stage              = find_string("stage");
    out.duration_ms        = find_double("duration_ms");
    out.resolved_imports   = find_u64("resolved_imports");
    out.unresolved_imports = find_u64("unresolved_imports");
    return !out.schema_version.empty();
}

} // namespace

std::vector<CompatSummary> LoadCompatHistory(const std::string& dir,
                                             const std::string& title_id,
                                             std::size_t max_entries) {
    std::vector<CompatSummary> out;
    if (title_id.empty() || max_entries == 0) return out;
    const std::string path = dir + "/compat/" + title_id + ".jsonl";
    std::ifstream in(path, std::ios::binary);
    if (!in) return out;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        CompatSummary s;
        if (ParseSummaryLine(line, s)) {
            out.push_back(std::move(s));
        }
    }
    // Caller asked for most-recent first.  We just read the file sequentially
    // and reverse here.
    if (out.size() > max_entries) {
        out.erase(out.begin(),
                  out.begin() + static_cast<std::ptrdiff_t>(out.size() - max_entries));
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// ===========================================================================
// Regression analysis
// ===========================================================================
RegressionEntry EvaluateRegression(const std::vector<CompatSummary>& history,
                                    const CompatSummary& current) {
    RegressionEntry e;
    e.summary = current;
    if (current.title_id.empty()) e.summary.title_id = SanitiseTitle(current.target);

    // history is already most-recent-first.
    if (history.empty()) {
        e.verdict = "new";
        e.baseline_samples = 0;
        return e;
    }

    double sum = 0.0, mn = history[0].duration_ms, mx = history[0].duration_ms;
    std::size_t ok = 0;
    for (const auto& h : history) {
        sum += h.duration_ms;
        mn = std::min(mn, h.duration_ms);
        mx = std::max(mx, h.duration_ms);
        if (!h.status.empty()) ++ok;
    }
    e.baseline_samples   = history.size();
    e.baseline_avg_ms    = sum / static_cast<double>(history.size());
    e.baseline_min_ms    = mn;
    e.baseline_max_ms    = mx;
    e.delta_ms           = current.duration_ms - e.baseline_avg_ms;
    e.delta_pct          = e.baseline_avg_ms > 0.0
                            ? (e.delta_ms / e.baseline_avg_ms) * 100.0
                            : 0.0;

    // Status change dominates timing.
    const std::string& prev_status = history.front().status;
    if (current.status == "fail" && prev_status != "fail") {
        e.verdict = "regression";
        return e;
    }
    if (current.status == "pass" && prev_status == "fail") {
        e.verdict = "improvement";
        return e;
    }
    const double kThresholdPct = 10.0;
    if (e.delta_pct > kThresholdPct) {
        e.verdict = "regression";
    } else if (e.delta_pct < -kThresholdPct) {
        e.verdict = "improvement";
    } else {
        e.verdict = "stable";
    }
    return e;
}

// ===========================================================================
// Aggregated markdown report
// ===========================================================================
std::string BuildRegressionMarkdown(const std::vector<RegressionEntry>& entries) {
    std::ostringstream os;
    os << "# pcsx5 regression report\n\n";
    os << "- schema_version: pcsx5.regression.v" << kCurrentSchemaVersion << "\n";
    os << "- generated:     " << IsoNow() << "\n";
    os << "- entries:       " << entries.size() << "\n";

    int regressions  = 0, improvements = 0, stable = 0, fresh = 0;
    for (const auto& e : entries) {
        if      (e.verdict == "regression")  ++regressions;
        else if (e.verdict == "improvement") ++improvements;
        else if (e.verdict == "stable")      ++stable;
        else if (e.verdict == "new")         ++fresh;
    }
    os << "- regressions:   " << regressions  << "\n";
    os << "- improvements:  " << improvements << "\n";
    os << "- stable:        " << stable      << "\n";
    os << "- new titles:    " << fresh       << "\n\n";

    os << "| title | status | verdict | duration (ms) | baseline avg | delta % | samples |\n";
    os << "| ----- | ------ | ------- | ------------: | -----------: | ------: | ------: |\n";
    for (const auto& e : entries) {
        const auto& s = e.summary;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f", s.duration_ms);
        std::string dur = buf;
        std::snprintf(buf, sizeof(buf), "%.2f", e.baseline_avg_ms);
        std::string base = buf;
        std::snprintf(buf, sizeof(buf), "%+.1f", e.delta_pct);
        std::string delta = buf;
        os << "| " << (s.title_id.empty() ? "(unnamed)" : s.title_id)
           << " | " << s.status
           << " | " << e.verdict
           << " | " << dur
           << " | " << base
           << " | " << delta
           << " | " << e.baseline_samples << " |\n";
    }
    return os.str();
}

bool WriteRegressionMarkdown(const std::string& path,
                             const std::vector<RegressionEntry>& entries,
                             std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::trunc | std::ios::binary);
    if (!out) {
        if (error) *error = "could not open regression report for writing";
        return false;
    }
    out << BuildRegressionMarkdown(entries);
    return out.good();
}

} // namespace Reports
