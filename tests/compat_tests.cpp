// Unit tests for the per-title compatibility database.
//
//   - Status name round-trip
//   - Save / Load round-trip preserves every curated + auto field
//   - UpdateAuto only touches the auto block (curated_at is preserved)
//   - ListTitles / Search honour the index
//   - WriteMarkdownReport produces a non-empty table
//
// Self-contained: uses a per-test scratch dir under std::filesystem::temp_directory_path().

#include "compat/compat.h"
#include "common/log.h"
#include "common/types.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

int g_failures = 0;
int g_checks   = 0;

#define EXPECT(cond, msg) do {                                     \
    ++g_checks;                                                    \
    if (!(cond)) {                                                 \
        ++g_failures;                                              \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n",                  \
                     __FILE__, __LINE__, (msg));                   \
    }                                                              \
} while (0)

std::string MakeScratchDir() {
    auto base = std::filesystem::temp_directory_path();
    auto sub = base / ("pcsx5_compat_test_" + std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(sub, ec);
    std::filesystem::create_directories(sub, ec);
    return sub.string();
}

void TestStatusRoundTrip() {
    for (int i = 0; i < 5; ++i) {
        const auto s = static_cast<Compat::Status>(i);
        const char* n = Compat::StatusName(s);
        Compat::Status back;
        EXPECT(Compat::StatusFromName(n, back), "status round-trip parse");
        EXPECT(back == s, "status round-trip equality");
    }
    Compat::Status junk;
    EXPECT(!Compat::StatusFromName("nonsense", junk), "rejects unknown status");
}

Compat::Entry MakeSample(const std::string& title_id) {
    Compat::Entry e;
    e.title_id    = title_id;
    e.name        = "Sample Game";
    e.region      = "EU";
    e.version     = "1.00";
    e.status      = Compat::Status::Playable;
    e.notes       = "Test entry";
    e.workarounds = {"--strict-imports off", "--renderer 0"};
    e.curated_at  = "2026-07-13T10:00:00Z";
    e.auto_fields.last_tested                = "2026-07-13T11:00:00Z";
    e.auto_fields.last_run_status            = "pass";
    e.auto_fields.last_run_git_revision      = "abc1234";
    e.auto_fields.last_run_duration_ms       = 12.5;
    e.auto_fields.last_run_resolved_imports  = 480;
    e.auto_fields.last_run_unresolved_imports= 5;
    return e;
}

void TestSaveLoadRoundTrip(const std::string& dir) {
    Compat::Initialize(dir);
    auto e = MakeSample("PPSA00001");
    EXPECT(Compat::Save(std::move(e), nullptr), "Save returns true");

    Compat::Entry loaded;
    EXPECT(Compat::Load("PPSA00001", loaded, nullptr), "Load returns true");
    EXPECT(loaded.title_id == "PPSA00001", "title_id preserved");
    EXPECT(loaded.name     == "Sample Game", "name preserved");
    EXPECT(loaded.region   == "EU", "region preserved");
    EXPECT(loaded.version  == "1.00", "version preserved");
    EXPECT(loaded.status   == Compat::Status::Playable, "status preserved");
    EXPECT(loaded.notes    == "Test entry", "notes preserved");
    EXPECT(loaded.curated_at == "2026-07-13T10:00:00Z", "curated_at preserved");
    EXPECT(loaded.workarounds.size() == 2, "workarounds count");
    EXPECT(loaded.workarounds[0] == "--strict-imports off", "workarounds[0]");
    EXPECT(loaded.auto_fields.last_run_status == "pass", "auto.last_run_status");
    EXPECT(loaded.auto_fields.last_run_resolved_imports == 480, "auto.resolved");
    EXPECT(loaded.auto_fields.last_run_unresolved_imports == 5, "auto.unresolved");
    EXPECT(loaded.auto_fields.last_run_git_revision == "abc1234", "auto.git_rev");
    EXPECT(loaded.auto_fields.last_run_duration_ms == 12.5, "auto.duration");
}

void TestUpdateAuto(const std::string& dir) {
    Compat::Initialize(dir);
    auto e = MakeSample("PPSA00002");
    e.curated_at = "2026-07-13T10:00:00Z";
    EXPECT(Compat::Save(std::move(e), nullptr), "Save returns true");

    Compat::AutoFields a;
    a.last_tested                  = "2026-07-13T20:00:00Z";
    a.last_run_status              = "fail";
    a.last_run_git_revision        = "deadbeef";
    a.last_run_duration_ms         = 99.0;
    a.last_run_resolved_imports    = 100;
    a.last_run_unresolved_imports  = 50;
    EXPECT(Compat::UpdateAuto("PPSA00002", a, nullptr), "UpdateAuto returns true");

    Compat::Entry loaded;
    EXPECT(Compat::Load("PPSA00002", loaded, nullptr), "Load after UpdateAuto");
    EXPECT(loaded.name == "Sample Game", "UpdateAuto preserves name");
    EXPECT(loaded.curated_at == "2026-07-13T10:00:00Z", "UpdateAuto preserves curated_at");
    EXPECT(loaded.auto_fields.last_run_status == "fail", "auto.status updated");
    EXPECT(loaded.auto_fields.last_run_git_revision == "deadbeef", "auto.git updated");
    EXPECT(loaded.auto_fields.last_run_unresolved_imports == 50, "auto.unresolved updated");
}

void TestListAndSearch(const std::string& dir) {
    Compat::Initialize(dir);
    EXPECT(Compat::Save(MakeSample("PPSA00001"), nullptr), "Save 001");
    {
        auto e = MakeSample("PPSA00002");
        e.name = "Another Game";
        e.region = "US";
        e.status = Compat::Status::Menu;
        EXPECT(Compat::Save(std::move(e), nullptr), "Save 002");
    }
    {
        auto e = MakeSample("PPSA00003");
        e.name = "Third Game";
        e.region = "JP";
        e.status = Compat::Status::Complete;
        EXPECT(Compat::Save(std::move(e), nullptr), "Save 003");
    }

    auto titles = Compat::ListTitles();
    EXPECT(titles.size() == 3, "ListTitles returns 3");
    EXPECT(titles[0] == "PPSA00001", "titles[0]");
    EXPECT(titles[2] == "PPSA00003", "titles[2]");

    auto all = Compat::Search("");
    EXPECT(all.size() == 3, "Search empty returns all");

    auto matches = Compat::Search("Another");
    EXPECT(matches.size() == 1, "Search 'Another' returns 1");
    EXPECT(matches[0].title_id == "PPSA00002", "Search hits PPSA00002");

    auto by_status = Compat::Search("complete");
    EXPECT(by_status.size() == 1, "Search by status 'complete' returns 1");
    EXPECT(by_status[0].title_id == "PPSA00003", "status search hit");

    auto by_region = Compat::Search("JP");
    EXPECT(by_region.size() == 1, "Search by region 'JP' returns 1");
}

void TestRemoveAndReAdd(const std::string& dir) {
    Compat::Initialize(dir);
    EXPECT(Compat::Save(MakeSample("PPSA00099"), nullptr), "Save 099");
    EXPECT(Compat::Find("PPSA00099") != nullptr, "Find after save");
    EXPECT(Compat::Remove("PPSA00099", nullptr), "Remove");
    EXPECT(Compat::Find("PPSA00099") == nullptr, "Find after remove");

    EXPECT(Compat::Save(MakeSample("PPSA00099"), nullptr), "Re-save after remove");
    EXPECT(Compat::Find("PPSA00099") != nullptr, "Find after re-save");
}

void TestMarkdownReport(const std::string& dir) {
    Compat::Initialize(dir);
    EXPECT(Compat::Save(MakeSample("PPSA00001"), nullptr), "Save 001 for report");
    const std::string out = dir + "/report.md";
    std::string err;
    EXPECT(Compat::WriteMarkdownReport(out, &err), "WriteMarkdownReport");
    EXPECT(std::filesystem::exists(out), "report.md exists");
    std::ifstream f(out);
    std::string body((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    EXPECT(body.find("PPSA00001") != std::string::npos, "report contains title_id");
    EXPECT(body.find("Sample Game") != std::string::npos, "report contains name");
    EXPECT(body.find("playable") != std::string::npos, "report contains status");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    LogConfig::SetLevel(LogCategory::General, LogLevel::Warn);

    const std::string dir = MakeScratchDir();
    std::fprintf(stdout, "scratch dir: %s\n", dir.c_str());

    TestStatusRoundTrip();
    TestSaveLoadRoundTrip(dir + "/round_trip");
    TestUpdateAuto(dir + "/update_auto");
    TestListAndSearch(dir + "/list_search");
    TestRemoveAndReAdd(dir + "/remove");
    TestMarkdownReport(dir + "/report");

    std::fprintf(stdout, "\n  %d/%d checks passed\n",
                 g_checks - g_failures, g_checks);
    if (g_failures != 0) {
        std::fprintf(stderr, "  %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stdout, "OK\n");
    return 0;
}
