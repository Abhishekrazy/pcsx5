// Diagnostics subsystem tests — exercises the structured logging ring buffer,
// the HLE import-call trace, and the crash-report bundle writer (without
// actually crashing the process).
//
// Build target: diagnostics_tests (see CMakeLists.txt).

#include "common/log.h"
#include "memory/memory.h"
#include "diagnostics/diagnostics.h"
#include "hle/hle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

extern "C" u64 HleDispatch(u64, u64, u64, u64, u64, u64, u64, u64);

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

std::string Slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}

bool Contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// 1. Log ring buffer push / snapshot / clear / file output
// ---------------------------------------------------------------------------
void TestLogRingBuffer() {
    std::fprintf(stdout, "[TEST] Log ring buffer\n");
    ClearRecentLogEntries();
    EXPECT(GetRecentLogEntries().empty(), "fresh ring is empty");

    LOG_INFO(General, "ring-test %d", 1);
    LOG_INFO(General, "ring-test %d", 2);
    LOG_INFO(General, "ring-test %d", 3);

    auto entries = GetRecentLogEntries();
    EXPECT_EQ(entries.size(), (size_t)3, "three entries pushed");
    EXPECT(Contains(entries[0].message, "ring-test 1"), "first message");
    EXPECT(Contains(entries[2].message, "ring-test 3"), "third message");
    EXPECT_EQ(entries[0].category, LogCategory::General, "category preserved");
    EXPECT_EQ(entries[0].level, LogLevel::Info, "level preserved");
    EXPECT(!entries[0].file.empty(), "file captured");
    EXPECT(entries[0].line > 0, "line captured");
    EXPECT(!entries[0].function.empty(), "function captured");
    EXPECT(entries[1].timestamp_us >= entries[0].timestamp_us, "monotonic timestamps");

    ClearRecentLogEntries();
    EXPECT(GetRecentLogEntries().empty(), "ring cleared");
}

// ---------------------------------------------------------------------------
// 2. Log file output
// ---------------------------------------------------------------------------
void TestLogFileOutput() {
    std::fprintf(stdout, "[TEST] Log file output\n");
    const std::string path = "diagnostics_test_log.txt";
    std::filesystem::remove(path);

    LogConfig::SetFileOutput(path, /*append=*/false);
    LOG_INFO(General, "file-output-line-1");
    LOG_WARN(General, "file-output-line-2");
    LogConfig::SetFileOutput(""); // disable

    std::string contents = Slurp(path);
    EXPECT(Contains(contents, "[General][Info] file-output-line-1"),
           "info line in file");
    EXPECT(Contains(contents, "[General][Warn] file-output-line-2"),
           "warn line in file");
    std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// 3. HLE import-call trace
// ---------------------------------------------------------------------------
void TestHleImportTrace() {
    std::fprintf(stdout, "[TEST] HLE import-call trace\n");
    if (!Memory::Initialize()) {
        std::fprintf(stderr, "FATAL: Memory::Initialize failed\n");
        ++g_failures; return;
    }
    if (!HLE::Initialize()) {
        std::fprintf(stderr, "FATAL: HLE::Initialize failed\n");
        ++g_failures; return;
    }
    HLE::SetStrictImportMode(false);
    HLE::ResetRunStatistics();

    HLE::RegisterSymbol("diagtest", "foo#T#T", [](const HLE::GuestArgs&) -> u64 { return 1; });
    guest_addr_t thunk = HLE::Resolve("diagtest", "foo#T#T");
    EXPECT(thunk != 0, "Registered symbol resolves");

    // Read the symbol id from the thunk
    u8 buf[10] = {};
    std::memcpy(buf, reinterpret_cast<const void*>(thunk), sizeof(buf));
    u64 symbol_id = 0;
    for (int i = 0; i < 8; ++i) {
        symbol_id |= static_cast<u64>(buf[2 + i]) << (8 * i);
    }
    EXPECT(symbol_id != 0, "Extracted symbol id");

    HLE::ClearImportTrace();
    HleDispatch(symbol_id, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0xCAFE);
    HleDispatch(symbol_id, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0xBEEF);

    auto trace = HLE::GetImportTrace();
    EXPECT_EQ(trace.size(), (size_t)2, "two trace entries");
    EXPECT_STR_EQ(trace[0].module_name, std::string("diagtest"), "module captured");
    EXPECT_STR_EQ(trace[0].name,        std::string("foo#T#T"), "name captured");
    EXPECT_EQ(trace[0].arg1,        (u64)0xAA,               "arg1 captured");
    EXPECT_EQ(trace[0].caller_rip,  (u64)0xCAFE,             "caller captured");
    EXPECT_EQ(trace[1].arg1,        (u64)0x11,               "arg1 of second call");
    EXPECT_EQ(trace[1].caller_rip,  (u64)0xBEEF,             "caller of second call");

    HLE::Shutdown();
    Memory::Shutdown();
}

// ---------------------------------------------------------------------------
// 4. Crash report bundle layout (force-written without a real crash)
// ---------------------------------------------------------------------------
void TestCrashReportBundleLayout() {
    std::fprintf(stdout, "[TEST] Crash report bundle layout\n");

    const std::string dir = "diagnostics_test_crash";
    std::filesystem::remove_all(dir);

    Diagnostics::InstallCrashHandler(dir);
    EXPECT_STR_EQ(Diagnostics::BundleDirectory(), dir, "bundle dir is configured");

    // We can't easily simulate the SEH filter from a unit test (the filter
    // captures a real CONTEXT), so we just verify the directory was created
    // by InstallCrashHandler and that the public bundle API reports a stub
    // path correctly when no crash has been recorded.
    std::string written = Diagnostics::WriteCrashReportBundle(/*force=*/false);
    EXPECT(written.empty(), "no bundle without a crash + force");

    EXPECT(std::filesystem::exists(dir), "bundle directory was created");

    Diagnostics::ResetCrashReport();
    EXPECT(!Diagnostics::HasCrashReport(), "no crash report after reset");
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    TestLogRingBuffer();
    TestLogFileOutput();
    TestHleImportTrace();
    TestCrashReportBundleLayout();

    std::fprintf(stdout, "Diagnostics: %d check(s), %d failure(s)\n",
                 g_checks, g_failures);
    if (g_failures == 0) {
        std::fprintf(stdout, "Diagnostics tests passed.\n");
        std::error_code ec;
        std::filesystem::remove_all("diagnostics_test_crash", ec);
        return 0;
    }
    std::fprintf(stderr, "Diagnostics tests FAILED with %d failure(s).\n", g_failures);
    return 1;
}
