// NID database file (LoadNidDatabase) tests.
//
// Verifies:
//   - A temp DB file parses and its entries resolve via LookupNidName.
//   - Comment and blank lines are ignored.
//   - Malformed lines are skipped without crashing or aborting the load.
//   - Built-in table entries still resolve after a DB file is loaded.
//   - File entries take precedence over built-ins on NID collision.
//   - A missing file returns false.
//   - NIDs with a "#T#T"-style type tag are accepted and stripped.
//
// Self-contained: no Memory / Loader / HLE dependencies.

#include "common/nid.h"
#include "common/log.h"
#include "common/types.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

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

#define EXPECT_EQ(a, b, msg) do {                                  \
    ++g_checks;                                                    \
    if (!((a) == (b))) {                                           \
        ++g_failures;                                              \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n",                  \
                     __FILE__, __LINE__, (msg));                   \
    }                                                              \
} while (0)

void ExpectLookup(std::string_view nid11, std::string_view expected) {
    auto nid = Common::DecodeNid(nid11);
    EXPECT(nid.has_value(), "DecodeNid of test NID failed");
    if (!nid) return;
    auto name = Common::LookupNidName(*nid);
    EXPECT(name.has_value(), "LookupNidName missed expected entry");
    if (name) {
        EXPECT_EQ(*name, expected, "lookup returned wrong name");
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    LogConfig::SetLevel(LogCategory::Loader, LogLevel::Error);

    std::fprintf(stdout, "=== nid_db_tests ===\n");

    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "pcsx5_nid_db_test.txt";

    // 1. Missing file returns false.
    {
        std::error_code ec;
        fs::remove(tmp, ec);
        EXPECT(!Common::LoadNidDatabase(tmp), "missing file should return false");
    }

    // 2. Write a DB file exercising every format feature.
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << "# comment line\n";
        out << "\n";
        out << "   \n";
        out << "00000000000\tlibkernel\tfileOnlySymbol\n";
        // NID with type tag: tag must be accepted and stripped.
        out << "zzzzzzzzzzw#T#T\tlibScePad\tfileTaggedSymbol\n";
        // Malformed lines: missing fields, invalid NID, empty name.
        out << "not-a-valid-nid\n";
        out << "AAAAAAAAAAA\tlibkernel\n";
        out << "AAA\tlibkernel\ttooShortNid\n";
        out << "AAAAAAAAAAA\tlibkernel\t\n";
        // Override of a built-in NID: file takes precedence.
        out << "pZ9WXcClPO8\tlibkernel\toverriddenName\n";
        out.close();
        EXPECT(out.good(), "failed to write temp DB file");

        EXPECT(Common::LoadNidDatabase(tmp), "valid file should return true");
    }

    // 3. File-only entries resolve (bare and tagged forms).
    ExpectLookup("00000000000", "fileOnlySymbol");
    ExpectLookup("zzzzzzzzzzw", "fileTaggedSymbol");

    // 4. Malformed lines were skipped without crashing (their NIDs miss).
    {
        auto nid = Common::DecodeNid("AAAAAAAAAAA");
        EXPECT(nid.has_value(), "decode failed");
        if (nid) {
            EXPECT(!Common::LookupNidName(*nid).has_value(),
                   "malformed lines should not register entries");
        }
    }

    // 5. Precedence: the file entry overrode the built-in name.
    ExpectLookup("pZ9WXcClPO8", "overriddenName");

    // 6. Built-in entries not present in the file still resolve.
    ExpectLookup("byV+FWlAnB4", "sceKernelMapFlexibleMemory");
    ExpectLookup("+P6FRGH4LfA", "memmove");
    EXPECT(Common::IsKnownNid("9ByRMdo7ywg#T#T"),
           "IsKnownNid should still hit built-in entries");

    // 7. A second load merges alongside the first.
    {
        const fs::path tmp2 = fs::temp_directory_path() / "pcsx5_nid_db_test2.txt";
        {
            std::ofstream out(tmp2, std::ios::binary | std::ios::trunc);
            out << "M3aXkQY3e9A\tlibkernel\tsecondFileSymbol\n";
        }
        EXPECT(Common::LoadNidDatabase(tmp2), "second file should load");
        ExpectLookup("M3aXkQY3e9A", "secondFileSymbol");
        ExpectLookup("00000000000", "fileOnlySymbol");  // first file still there
        std::error_code ec;
        fs::remove(tmp2, ec);
    }

    std::error_code ec;
    fs::remove(tmp, ec);

    std::fprintf(stdout, "  %d/%d checks passed\n",
                 g_checks - g_failures, g_checks);
    if (g_failures != 0) {
        std::fprintf(stderr, "  %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stdout, "OK\n");
    return 0;
}
