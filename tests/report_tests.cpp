// Reports module tests — CompatSummary serialisation, per-title jsonl
// history, regression verdict, and aggregated markdown report.

#include "reports/reports.h"
#include "common/log.h"
#include "hle/hle.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks   = 0;

#define EXPECT(cond, msg)                                                            \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg);    \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

#define EXPECT_EQ(a, b, msg)                                                                    \
    do {                                                                                        \
        ++g_checks;                                                                            \
        auto _lhs = (a);                                                                        \
        auto _rhs = (b);                                                                        \
        if (!(_lhs == _rhs)) {                                                                  \
            std::fprintf(stderr, "[FAIL] %s:%d: %s  (lhs=%lld rhs=%lld)\n",                     \
                         __FILE__, __LINE__, msg,                                               \
                         (long long)_lhs, (long long)_rhs);                                     \
            ++g_failures;                                                                       \
        }                                                                                       \
    } while (0)

#define EXPECT_STR_EQ(a, b, msg)                                                                \
    do {                                                                                        \
        ++g_checks;                                                                            \
        std::string _lhs = (a);                                                                 \
        std::string _rhs = (b);                                                                 \
        if (_lhs != _rhs) {                                                                     \
            std::fprintf(stderr, "[FAIL] %s:%d: %s  (lhs=\"%s\" rhs=\"%s\")\n",                 \
                         __FILE__, __LINE__, msg, _lhs.c_str(), _rhs.c_str());                  \
            ++g_failures;                                                                       \
        }                                                                                       \
    } while (0)

Reports::CompatSummary MakeSummary(const std::string& title, const std::string& status,
                                   double dur_ms) {
    Reports::CompatSummary s;
    s.title_id           = title;
    s.target             = "eboot/" + title + ".elf";
    s.status             = status;
    s.stage              = "execute";
    s.duration_ms        = dur_ms;
    s.resolved_imports   = 5;
    s.unresolved_imports = 0;
    s.timestamp_iso      = "2026-07-13T00:00:00Z";
    s.git_revision       = "deadbeef";
    HLE::ImportStats imp;
    imp.module_name     = "libkernel";
    imp.name            = "sceKernelExit";
    imp.call_count      = 1;
    imp.thunk_address   = 0x1000;
    imp.last_caller_rip = 0x2000;
    s.top_imports.push_back(imp);
    return s;
}

// ---------------------------------------------------------------------------
// 1. CompatSummary serialises to a parseable jsonl line
// ---------------------------------------------------------------------------
// Per-process scratch directory under the system temp dir.  Using a stable
// absolute path keeps the test deterministic regardless of the cwd chosen by
// the test runner (e.g. ctest vs. direct invocation).
std::string ScratchDir() {
    auto base = std::filesystem::temp_directory_path() / "pcsx5_report_test";
    return base.string();
}

void TestSummarySerialisation() {
    std::fprintf(stdout, "[TEST] CompatSummary serialisation\n");
    Reports::CompatSummary s = MakeSummary("CUSA00001", "pass", 12.5);
    s.title_id = "CUSA00001";
    std::string line = Reports::SerializeSummaryJsonl(s);
    EXPECT(!line.empty(), "non-empty line");
    EXPECT(line.find("\"schema_version\"") != std::string::npos, "schema_version present");
    EXPECT(line.find("\"title_id\":\"CUSA00001\"") != std::string::npos, "title_id present");
    EXPECT(line.find("\"status\":\"pass\"") != std::string::npos, "status present");
    EXPECT(line.find("libkernel") != std::string::npos, "top import module present");
    EXPECT(line.find('\n') == std::string::npos, "single line, no embedded newline");
    EXPECT(line.back() != '\n', "no trailing newline in single line");
}

// ---------------------------------------------------------------------------
// 2. AppendCompatHistory + LoadCompatHistory round-trip
// ---------------------------------------------------------------------------
void TestHistoryAppendAndLoad() {
    std::fprintf(stdout, "[TEST] CompatSummary history round-trip\n");
    const std::string dir = ScratchDir();
    std::filesystem::remove_all(dir);

    auto append = [&](const std::string& title, double dur) {
        Reports::CompatSummary s = MakeSummary(title, "pass", dur);
        std::string err;
        std::string p = Reports::AppendCompatHistory(dir, s, &err);
        EXPECT(!p.empty(), "append succeeds");
    };
    append("CUSA00001", 10.0);
    append("CUSA00001", 20.0);
    append("CUSA00001", 30.0);

    auto history = Reports::LoadCompatHistory(dir, "CUSA00001", 32);
    EXPECT_EQ(history.size(), (size_t)3, "three entries loaded");
    // LoadCompatHistory returns most-recent-first
    EXPECT(history[0].duration_ms > history[1].duration_ms, "most-recent first");
    EXPECT(history[2].duration_ms < history[0].duration_ms, "oldest last");

    // max_entries clamps
    auto clamped = Reports::LoadCompatHistory(dir, "CUSA00001", 2);
    EXPECT_EQ(clamped.size(), (size_t)2, "max_entries clamps");

    // Missing file -> empty vector
    auto missing = Reports::LoadCompatHistory(dir, "UNKNOWN", 32);
    EXPECT(missing.empty(), "missing title returns empty");

    // WriteCompatSummary round-trips a standalone file
    const std::string one_path = dir + "/single.json";
    Reports::CompatSummary s = MakeSummary("CUSA00001", "pass", 7.0);
    EXPECT(Reports::WriteCompatSummary(one_path, s), "WriteCompatSummary succeeds");
    EXPECT(std::filesystem::exists(one_path), "summary file exists");
}

// ---------------------------------------------------------------------------
// 3. Regression evaluation
// ---------------------------------------------------------------------------
void TestRegressionEvaluation() {
    std::fprintf(stdout, "[TEST] Regression evaluation\n");

    // (a) Empty history -> "new"
    {
        std::vector<Reports::CompatSummary> hist;
        auto cur = MakeSummary("CUSA00001", "pass", 10.0);
        auto e = Reports::EvaluateRegression(hist, cur);
        EXPECT_STR_EQ(e.verdict, std::string("new"), "empty history -> new");
    }
    // (b) Within +/- 10% -> "stable"
    {
        std::vector<Reports::CompatSummary> hist = {
            MakeSummary("CUSA00001", "pass", 100.0),
            MakeSummary("CUSA00001", "pass", 100.0),
        };
        auto cur = MakeSummary("CUSA00001", "pass", 105.0);
        auto e = Reports::EvaluateRegression(hist, cur);
        EXPECT_STR_EQ(e.verdict, std::string("stable"), "+5% -> stable");
        EXPECT_EQ(e.baseline_samples, (size_t)2, "baseline samples counted");
    }
    // (c) Improvement: -30%
    {
        std::vector<Reports::CompatSummary> hist = {
            MakeSummary("CUSA00001", "pass", 100.0),
        };
        auto cur = MakeSummary("CUSA00001", "pass", 70.0);
        auto e = Reports::EvaluateRegression(hist, cur);
        EXPECT_STR_EQ(e.verdict, std::string("improvement"), "-30% -> improvement");
    }
    // (d) Regression: +30%
    {
        std::vector<Reports::CompatSummary> hist = {
            MakeSummary("CUSA00001", "pass", 100.0),
        };
        auto cur = MakeSummary("CUSA00001", "pass", 130.0);
        auto e = Reports::EvaluateRegression(hist, cur);
        EXPECT_STR_EQ(e.verdict, std::string("regression"), "+30% -> regression");
        EXPECT(e.delta_pct > 0.0, "delta_pct positive on regression");
    }
    // (e) Status change pass -> fail always regresses
    {
        std::vector<Reports::CompatSummary> hist = {
            MakeSummary("CUSA00001", "pass", 100.0),
        };
        auto cur = MakeSummary("CUSA00001", "fail", 100.0);
        auto e = Reports::EvaluateRegression(hist, cur);
        EXPECT_STR_EQ(e.verdict, std::string("regression"), "pass->fail = regression");
    }
    // (f) Status change fail -> pass is an improvement
    {
        std::vector<Reports::CompatSummary> hist = {
            MakeSummary("CUSA00001", "fail", 100.0),
        };
        auto cur = MakeSummary("CUSA00001", "pass", 100.0);
        auto e = Reports::EvaluateRegression(hist, cur);
        EXPECT_STR_EQ(e.verdict, std::string("improvement"), "fail->pass = improvement");
    }
}

// ---------------------------------------------------------------------------
// 4. Aggregated markdown report
// ---------------------------------------------------------------------------
void TestAggregatedReport() {
    std::fprintf(stdout, "[TEST] Aggregated regression markdown\n");
    std::vector<Reports::RegressionEntry> entries;
    {
        std::vector<Reports::CompatSummary> hist = { MakeSummary("CUSA00001", "pass", 100.0) };
        auto e = Reports::EvaluateRegression(hist, MakeSummary("CUSA00001", "pass", 130.0));
        entries.push_back(e);
    }
    {
        std::vector<Reports::CompatSummary> hist; // no history
        auto e = Reports::EvaluateRegression(hist, MakeSummary("CUSA00002", "pass", 10.0));
        entries.push_back(e);
    }
    {
        std::vector<Reports::CompatSummary> hist = { MakeSummary("CUSA00003", "fail", 50.0) };
        auto e = Reports::EvaluateRegression(hist, MakeSummary("CUSA00003", "pass", 50.0));
        entries.push_back(e);
    }
    std::string md = Reports::BuildRegressionMarkdown(entries);
    EXPECT(md.find("# pcsx5 regression report") == 0, "report starts with the title");
    EXPECT(md.find("CUSA00001") != std::string::npos, "row for title 1");
    EXPECT(md.find("CUSA00002") != std::string::npos, "row for title 2");
    EXPECT(md.find("CUSA00003") != std::string::npos, "row for title 3");
    EXPECT(md.find("regressions:")  != std::string::npos, "regressions counter");
    EXPECT(md.find("improvements:") != std::string::npos, "improvements counter");
    EXPECT(md.find("new titles:")   != std::string::npos, "new titles counter");

    // Persist it
    const std::string path = ScratchDir() + "/regression.md";
    std::string err;
    EXPECT(Reports::WriteRegressionMarkdown(path, entries, &err), "write report");
    EXPECT(std::filesystem::exists(path), "report file exists");
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    TestSummarySerialisation();
    TestHistoryAppendAndLoad();
    TestRegressionEvaluation();
    TestAggregatedReport();

    std::fprintf(stdout, "Reports: %d check(s), %d failure(s)\n", g_checks, g_failures);
    // Always clean up the scratch directory so a previous failing run cannot
    // leak state into the next invocation.
    std::error_code ec;
    std::filesystem::remove_all(ScratchDir(), ec);
    if (g_failures == 0) {
        std::fprintf(stdout, "Reports tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "Reports tests FAILED with %d failure(s).\n", g_failures);
    return 1;
}
