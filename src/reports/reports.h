#pragma once
//
// Compatibility + regression report templates.
//
// Two layered outputs:
//
//  * `CompatSummary`  - the per-run verdict for one title, written to
//                       <dir>/compat/<title_id>.jsonl as one JSON object per
//                       line (one entry per run).  This is the regression
//                       data source.
//
//  * `RegressionEntry` - the per-run verdict plus a verdict against the
//                       recent history baseline (stable / improvement /
//                       regression / new).  Aggregated into a markdown
//                       regression report covering every tracked title.
//
// Both objects are intentionally plain structs so callers can fill them
// without going through a builder API.
//
#include "../hle/hle.h"
#include <cstddef>
#include <string>
#include <vector>

namespace Reports {

// Schema version of the JSON document.  Bump when fields are added/removed.
inline constexpr int kCurrentSchemaVersion = 1;

// Top-K most-called imports to record in the summary (sized to keep the
// document readable).
inline constexpr std::size_t kTopImports = 10;

// Per-run summary for one title.
struct CompatSummary {
    std::string                       title_id;        // e.g. CUSA00001
    std::string                       target;          // path to ELF / SELF
    std::string                       status;          // "pass" | "fail" | "error"
    std::string                       stage;           // "load" | "execute"
    double                            duration_ms = 0; // wall-clock
    u64                               resolved_imports  = 0;
    u64                               unresolved_imports = 0;
    std::string                       timestamp_iso;   // ISO-8601 UTC
    std::string                       schema_version;  // "pcsx5.compat.v1"
    // Caller-supplied git revision; empty if the binary wasn't built from a
    // known commit.  The runner is responsible for populating this.
    std::string                       git_revision;
    // Top-K HLE imports by call count, sorted descending.
    std::vector<HLE::ImportStats>     top_imports;

    // True when this summary has at least one resolved import.
    bool HasData() const { return resolved_imports > 0 || !top_imports.empty(); }
};

// One row in the regression report: the current run plus its verdict.
struct RegressionEntry {
    CompatSummary summary;             // the current run
    std::string   verdict;            // "new" | "stable" | "improvement" | "regression"
    double        baseline_avg_ms = 0; // mean duration over recent history
    double        baseline_min_ms = 0;
    double        baseline_max_ms = 0;
    std::size_t   baseline_samples = 0; // number of prior runs
    double        delta_ms = 0;         // current - baseline_avg_ms
    double        delta_pct = 0;        // (delta / baseline_avg) * 100
};

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

// Write a single CompatSummary as a pretty-printed JSON document.  The
// directory containing `path` is created if it does not exist.
bool WriteCompatSummary(const std::string& path, const CompatSummary& s,
                        std::string* error = nullptr);

// Append a CompatSummary to <dir>/compat/<title_id>.jsonl.  If `title_id` is
// empty, falls back to using a sanitised version of `target`.  Returns the
// absolute path to the jsonl file on success, empty string on failure.
std::string AppendCompatHistory(const std::string& dir,
                                CompatSummary s,
                                std::string* error = nullptr);

// Load the most recent N entries from the per-title jsonl file.  Returns an
// empty vector if the file does not exist.  Malformed lines are skipped.
std::vector<CompatSummary> LoadCompatHistory(const std::string& dir,
                                             const std::string& title_id,
                                             std::size_t max_entries = 32);

// ---------------------------------------------------------------------------
// Regression analysis
// ---------------------------------------------------------------------------

// Compute a regression entry for `current` against the given history.  The
// history is treated as ordered most-recent-first (matching LoadCompatHistory).
//   - history empty          -> verdict = "new"
//   - same status, +/- 10%   -> verdict = "stable"
//   - same status, <- 10%    -> verdict = "improvement"
//   - same status, >+ 10%    -> verdict = "regression"
//   - status changed to fail -> verdict = "regression"
//   - status changed to pass -> verdict = "improvement"
RegressionEntry EvaluateRegression(const std::vector<CompatSummary>& history,
                                   const CompatSummary& current);

// ---------------------------------------------------------------------------
// Aggregated regression report
// ---------------------------------------------------------------------------

// Write a markdown regression report summarising every entry.  Each entry
// becomes one row plus a short footer with aggregate counts.
bool WriteRegressionMarkdown(const std::string& path,
                             const std::vector<RegressionEntry>& entries,
                             std::string* error = nullptr);

// Build the markdown text without writing it.  Useful for tests and for
// callers that want to embed the report in something else.
std::string BuildRegressionMarkdown(const std::vector<RegressionEntry>& entries);

// Render a single CompatSummary as a single-line JSON object (no trailing
// newline).  The output is suitable for jsonl.  Exposed for tests.
std::string SerializeSummaryJsonl(const CompatSummary& s);

} // namespace Reports
