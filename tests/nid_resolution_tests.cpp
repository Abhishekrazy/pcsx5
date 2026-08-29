#include "hle/hle.h"
#include "hle/libkernel_sync.h"
#include "common/nid.h"
#include "common/log.h"
#include "memory/memory.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

int g_failures = 0;
int g_checks   = 0;

#define EXPECT(cond, msg) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, (msg)); \
    } \
} while (0)

#define EXPECT_EQ(a, b, msg) do { \
    ++g_checks; \
    if (!((a) == (b))) { \
        ++g_failures; \
        std::fprintf(stderr, "FAIL [%s:%d] %s (expected %llx, got %llx)\n", __FILE__, __LINE__, (msg), (u64)(b), (u64)(a)); \
    } \
} while (0)

void TestNidResolution(bool with_db) {
    std::fprintf(stdout, "[TEST] NID Resolution (with_db=%d)\n", with_db);
    
    HLE::ResetRunStatistics();
    
    guest_addr_t thunk_init = HLE::ResolveAny("cmo1RIYva9o#r#n");
    (void)thunk_init;
    if (with_db) {
        EXPECT(HLE::GetUnresolvedImportCount() == 0, "scePthreadMutexInit NID variant should resolve with db");
    } else {
        EXPECT(HLE::GetUnresolvedImportCount() != 0, "scePthreadMutexInit NID variant should NOT resolve without db");
    }
    
    guest_addr_t thunk_lock = HLE::ResolveAny("9UK1vLZQft4#y#J");
    (void)thunk_lock;
    EXPECT(HLE::GetUnresolvedImportCount() == (with_db ? 0 : 1), "scePthreadMutexLock NID variant should resolve");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    LogConfig::SetLevel(LogCategory::HLE, LogLevel::Error);

    Memory::Initialize();
    HLE::Initialize();
    
    HLE::RegisterLibKernelSync();

    std::fprintf(stdout, "=== nid_resolution_tests ===\n");

    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "pcsx5_nid_db_test_resolve.txt";

    // 1. Database absent
    {
        std::error_code ec;
        fs::remove(tmp, ec);
        TestNidResolution(false);
    }
    
    // 2. Database present
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << "cmo1RIYva9o\tlibkernel\tscePthreadMutexInit\n";
        out << "9UK1vLZQft4\tlibkernel\tscePthreadMutexLock\n";
        out.close();
        
        Common::LoadNidDatabase(tmp);
        TestNidResolution(true);
        
        std::error_code ec;
        fs::remove(tmp, ec);
    }

    std::fprintf(stdout, "  %d/%d checks passed\n", g_checks - g_failures, g_checks);
    if (g_failures != 0) {
        std::fprintf(stderr, "  %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stdout, "OK\n");
    return 0;
}
