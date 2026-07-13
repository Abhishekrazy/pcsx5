// Per-title compatibility database.
//
// Curated + auto schema stored as one JSON file per title:
//
//   <root>/
//     compatibility.json         <- index (title_id list + schema_version)
//     titles/<TITLE_ID>.json     <- full per-title entry
//
// Two field groups per entry:
//
//   * Curated fields (edited by humans via pcsx5_compat):
//       title_id, name, region, version, status, notes, workarounds,
//       curated_at
//
//   * Auto fields (written by the runtime via UpdateAuto after a run):
//       last_tested, last_run_status, last_run_git_revision,
//       last_run_duration_ms, last_run_unresolved_imports,
//       last_run_resolved_imports
//
// The CLI tool pcsx5_compat lives in tests/compat_tool.cpp and operates on
// this same API.

#pragma once

#include "../common/types.h"

#include <string>
#include <vector>

namespace Compat {

inline constexpr int kCurrentSchemaVersion = 1;

// Overall playability status.  Ordered: lower == worse.
enum class Status {
    Untested,   // never run
    Intro,      // gets to first logo / warning screen
    Menu,       // main menu reachable
    Playable,   // enters gameplay
    Complete    // beatable end-to-end
};

const char* StatusName(Status s);
bool        StatusFromName(const std::string& s, Status& out);

// Auto-derived fields (written by UpdateAuto).
struct AutoFields {
    std::string last_tested;                 // ISO-8601 UTC
    std::string last_run_status;             // "pass" | "fail" | "error"
    std::string last_run_git_revision;       // "" if unknown
    double      last_run_duration_ms = 0.0;
    u64         last_run_resolved_imports   = 0;
    u64         last_run_unresolved_imports = 0;
};

// One entry.
struct Entry {
    std::string              title_id;
    std::string              name;
    std::string              region;        // free-form, e.g. "EU", "US", "JP"
    std::string              version;       // free-form, e.g. "1.00", "01.020"
    Status                   status = Status::Untested;
    std::string              notes;
    std::vector<std::string> workarounds;   // CLI flags or notes
    std::string              curated_at;    // ISO-8601 UTC, set on save
    AutoFields               auto_fields;
};

// ---------------------------------------------------------------------------
// Service initialisation
// ---------------------------------------------------------------------------

// Bind the service to a root directory (created if missing).  Idempotent.
void        Initialize(const std::string& compat_dir);
const std::string& Directory();
bool        IsInitialized();

// ---------------------------------------------------------------------------
// Entry CRUD
// ---------------------------------------------------------------------------

// Returns the in-memory entry, or nullptr if not present.  Cache is
// populated lazily; call LoadAll() to pre-warm.
const Entry* Find(const std::string& title_id);

// Read a single entry from disk.  Does NOT touch the cache.
bool Load(const std::string& title_id, Entry& out, std::string* err = nullptr);

// Persist an entry.  Updates the index file.  Auto-populates `curated_at`
// if it is empty (use UpdateAuto to refresh the auto fields).
bool Save(Entry e, std::string* err = nullptr);

// Remove a title from the database.  Returns false if it was not present.
bool Remove(const std::string& title_id, std::string* err = nullptr);

// Update only the auto fields of an existing entry.  No-op (returns true)
// if the title is not present.
bool UpdateAuto(const std::string& title_id, const AutoFields& fields,
                std::string* err = nullptr);

// ---------------------------------------------------------------------------
// Listing / search
// ---------------------------------------------------------------------------

// All title ids known to the database, sorted alphabetically.
std::vector<std::string> ListTitles();

// Load every entry into memory and return them.  Sorted by title_id.
std::vector<Entry>        LoadAll();

// Free-text search across name, region, version, notes, status, title_id
// (case-insensitive).  Empty query returns every entry.
std::vector<Entry>        Search(const std::string& query);

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------

// Build a markdown table summarising every entry.  Sorted by title_id.
std::string BuildMarkdownTable(const std::vector<Entry>& entries);

// Write the markdown report to disk.  The parent directory of `path` is
// created if missing.
bool WriteMarkdownReport(const std::string& path, std::string* err = nullptr);

// ---------------------------------------------------------------------------
// Schema migration
// ---------------------------------------------------------------------------

// Migrate a raw `Entry` read from an older schema version to the current
// schema.  Always returns true (no-op for v1 -> v1).
bool MigrateToCurrent(int from_version, Entry& e, std::string* err = nullptr);

}  // namespace Compat
