// Unit tests for the HLE import reporting and strict-imports API.
// These tests exercise the registry, Resolve/ResolveAny, the auto-stub
// classification, and the per-symbol statistics without booting the full
// emulator.
//
// Build target: hle_import_report_tests (see CMakeLists.txt).

#include "hle/hle.h"
#include "memory/memory.h"
#include "common/log.h"
#include "common/nid.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" u64 HleDispatch(u64, u64, u64, u64, u64, u64, u64, u64, u64);

namespace {

// Read the symbol_id encoded in a thunk.  The thunk machine code is:
//   49 BA <8-byte symbol_id>   ; mov r10, symbol_id
//   48 B8 <8-byte dispatcher>  ; mov rax, HleCommonDispatcher
//   FF E0                       ; jmp rax
u64 ReadSymbolIdFromThunk(guest_addr_t thunk_addr) {
    if (!thunk_addr) return 0;
    u8 buf[10] = {};
    std::memcpy(buf, reinterpret_cast<const void*>(thunk_addr), sizeof(buf));
    // Skip 49 BA (2 bytes) and read the next 8 as little-endian u64
    u64 id = 0;
    for (int i = 0; i < 8; ++i) {
        id |= static_cast<u64>(buf[2 + i]) << (8 * i);
    }
    return id;
}

int g_failures = 0;

#define EXPECT(cond, msg)                                                        \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

#define EXPECT_EQ(a, b, msg)                                                                   \
    do {                                                                                       \
        auto _lhs = (a);                                                                       \
        auto _rhs = (b);                                                                       \
        if (!(_lhs == _rhs)) {                                                                 \
            std::fprintf(stderr, "[FAIL] %s:%d: %s  (lhs=%lld rhs=%lld)\n",                    \
                         __FILE__, __LINE__, msg,                                              \
                         (long long)_lhs, (long long)_rhs);                                    \
            ++g_failures;                                                                      \
        }                                                                                      \
    } while (0)

void ResetCounters() {
    HLE::ResetRunStatistics();
    EXPECT_EQ(HLE::GetUnresolvedImportCount(), (u64)0, "fresh unresolved count should be zero");
    EXPECT(HLE::GetImportReport().empty(), "fresh import report should be empty");
}

void TestResolveAndReport() {
    std::fprintf(stdout, "[TEST] Resolve, report, strict mode\n");

    HLE::SetStrictImportMode(false);
    ResetCounters();

    HLE::RegisterSymbol("testlib", "alpha#T#T", [](const HLE::GuestArgs& /*a*/) -> u64 { return 0xAA; });
    HLE::RegisterSymbol("testlib", "beta#T#T",  [](const HLE::GuestArgs& /*a*/) -> u64 { return 0xBB; });

    // Exact match returns a non-zero thunk address
    guest_addr_t a = HLE::Resolve("testlib", "alpha#T#T");
    EXPECT(a != 0, "Resolve alpha#T#T should succeed");

    // Base-NID variant match
    guest_addr_t a_var = HLE::Resolve("testlib", "alpha#j#j");
    EXPECT(a_var == a, "Resolve alpha#j#j should NID-variant match alpha#T#T");

    // Cross-module search
    guest_addr_t b_any = HLE::ResolveAny("beta#T#T");
    EXPECT(b_any != 0, "ResolveAny beta#T#T should succeed");

    // Read the actual symbol ids from the thunks (libraries registered before us)
    u64 alpha_id = ReadSymbolIdFromThunk(a);
    u64 beta_id  = ReadSymbolIdFromThunk(b_any);
    EXPECT(alpha_id != 0 && beta_id != 0, "thunks should encode non-zero symbol ids");
    EXPECT(alpha_id != beta_id, "alpha and beta must have different ids");

    // Dispatch to bump call counts.  HleDispatch is a real entrypoint — we
    // call it directly with the IDs we just read from the thunks.
    HleDispatch(/*symbol_id=*/alpha_id, /*rdi=*/0x11, /*rsi=*/0x22, /*rdx=*/0x33,
                /*rcx=*/0x44, /*r8=*/0x55, /*r9=*/0x66, /*guest_rip=*/0xDEADBEEF,
                /*guest_rsp=*/0);
    HleDispatch(alpha_id, 0, 0, 0, 0, 0, 0, 0xCAFE0001, 0);
    HleDispatch(beta_id,  0, 0, 0, 0, 0, 0, 0xCAFE0002, 0);

    auto report = HLE::GetImportReport();
    EXPECT_EQ(report.size(), (size_t)2, "report should contain both registered symbols");

    bool saw_alpha = false, saw_beta = false;
    for (const auto& s : report) {
        if (s.name == "alpha#T#T") {
            saw_alpha = true;
            EXPECT_EQ(s.call_count, (u64)2, "alpha call count");
            EXPECT_EQ(s.last_caller_rip, (u64)0xCAFE0001, "alpha last caller");
            EXPECT(s.module_name == "testlib", "alpha module");
        }
        if (s.name == "beta#T#T") {
            saw_beta = true;
            EXPECT_EQ(s.call_count, (u64)1, "beta call count");
            EXPECT_EQ(s.last_caller_rip, (u64)0xCAFE0002, "beta last caller");
        }
    }
    EXPECT(saw_alpha, "alpha in report");
    EXPECT(saw_beta,  "beta in report");
}

void TestJsonExport() {
    std::fprintf(stdout, "[TEST] JSON export and file writer\n");

    HLE::SetStrictImportMode(false);
    ResetCounters();

    // A NID present in the known-name table (sceKernelMapDirectMemory).
    HLE::RegisterSymbol("testlib", "L-Q3LEjIbgA#T#T",
                        [](const HLE::GuestArgs& /*a*/) -> u64 { return 0; });
    guest_addr_t known = HLE::Resolve("testlib", "L-Q3LEjIbgA#T#T");
    EXPECT(known != 0, "Resolve known NID should succeed");
    u64 known_id = ReadSymbolIdFromThunk(known);

    // An unknown NID — Resolve auto-stubs it.
    guest_addr_t stub = HLE::Resolve("testlib", "zzUnknownNid0#T#T");
    EXPECT(stub != 0, "unknown NID should auto-stub");
    u64 stub_id = ReadSymbolIdFromThunk(stub);
    EXPECT(HLE::GetUnresolvedImportCount() == 1, "one auto-stubbed import");

    HleDispatch(known_id, 0, 0, 0, 0, 0, 0, 0x1000, 0);
    HleDispatch(known_id, 0, 0, 0, 0, 0, 0, 0x1000, 0);
    HleDispatch(known_id, 0, 0, 0, 0, 0, 0, 0x1ABC, 0);
    HleDispatch(stub_id,  0, 0, 0, 0, 0, 0, 0x2000, 0);

    const std::string json_text = HLE::ExportImportReportJson();
    nlohmann::json arr = nlohmann::json::parse(json_text, nullptr, false);
    EXPECT(!arr.is_discarded(), "exported JSON should parse");
    EXPECT(arr.is_array(), "exported JSON should be an array");
    EXPECT_EQ(arr.size(), (size_t)2, "JSON should contain both called symbols");

    // Sorted by call_count descending: the known NID (3 calls) first.
    const auto& first = arr[0];
    EXPECT_EQ(first["call_count"].get<u64>(), (u64)3, "first entry call_count (sort order)");
    EXPECT(first["module"] == "testlib", "first entry module");
    EXPECT(first["nid"] == "L-Q3LEjIbgA#T#T", "first entry nid");
    EXPECT(first["name"] == "sceKernelMapDirectMemory", "known NID resolves to friendly name");
    EXPECT(first["auto_stubbed"] == false, "registered symbol is not auto-stubbed");
    EXPECT(first["last_caller_rip"] == "0x1abc", "last caller rip as hex string");

    const auto& second = arr[1];
    EXPECT_EQ(second["call_count"].get<u64>(), (u64)1, "second entry call_count");
    EXPECT(second["nid"] == "zzUnknownNid0#T#T", "second entry nid");
    EXPECT(second["name"] == "zzUnknownNid0#T#T", "unknown NID falls back to raw string");
    EXPECT(second["auto_stubbed"] == true, "auto-stubbed flag set for stubbed id");

    // WriteImportReportJson produces a parseable file with the same content.
    char tmp[MAX_PATH] = {};
    DWORD len = GetTempPathA(MAX_PATH, tmp);
    EXPECT(len > 0, "GetTempPathA should succeed");
    const std::string out_path = std::string(tmp) + "pcsx5_import_report_test.json";
    EXPECT(HLE::WriteImportReportJson(out_path), "WriteImportReportJson should succeed");

    std::ifstream in(out_path);
    std::stringstream ss;
    ss << in.rdbuf();
    in.close();
    nlohmann::json from_file = nlohmann::json::parse(ss.str(), nullptr, false);
    EXPECT(!from_file.is_discarded(), "written file should parse");
    EXPECT(from_file == arr, "written file should match in-memory export");
    std::remove(out_path.c_str());
}

void TestStrictModeFailsLoudly() {    std::fprintf(stdout, "[TEST] Strict-import mode refuses to stub\n");

    // Disable auto-stubbing.  A request for an unknown NID must return 0.
    HLE::SetStrictImportMode(true);
    ResetCounters();

    guest_addr_t r = HLE::ResolveAny("this_is_definitely_not_registered#T#T");
    EXPECT_EQ(r, (guest_addr_t)0, "strict-mode unknown NID should resolve to 0");

    // A known NID still works under strict mode.
    HLE::RegisterSymbol("testlib", "gamma#T#T", [](const HLE::GuestArgs& /*a*/) -> u64 { return 0xCC; });
    guest_addr_t g = HLE::Resolve("testlib", "gamma#T#T");
    EXPECT(g != 0, "strict-mode known NID should resolve");

    HLE::SetStrictImportMode(false);
}

void TestNewModulesResolve() {
    std::fprintf(stdout, "[TEST] Part-B modules resolve (savedata/user-service/dialog/agc/pad)\n");

    HLE::SetStrictImportMode(false);
    ResetCounters();

    // Module-name resolution for the canonical registrations.
    EXPECT(HLE::Resolve("libSceSaveData", "sceSaveDataInitialize3") != 0, "savedata init3 resolves");
    EXPECT(HLE::Resolve("libSceSaveData", "sceSaveDataMount3") != 0, "savedata mount3 resolves");
    EXPECT(HLE::Resolve("libSceUserService", "sceUserServiceGetLoginUserIdList") != 0, "user list resolves");
    EXPECT(HLE::Resolve("libSceUserService", "sceUserServiceGetUserName") != 0, "user name resolves");
    EXPECT(HLE::Resolve("libSceCommonDialog", "sceCommonDialogInitialize") != 0, "common dialog resolves");
    EXPECT(HLE::Resolve("libSceSaveDataDialog", "sceSaveDataDialogGetStatus") != 0, "dialog status resolves");
    EXPECT(HLE::Resolve("libSceAgc", "sceAgcGetRegisterDefaults2") != 0, "agc defaults2 resolves");

    // Games weakly import these through libkernel by NID — must not auto-stub.
    EXPECT(HLE::Resolve("libkernel", "TywrFKCoLGY#G#H") != 0, "savedata init3 via libkernel NID");
    EXPECT(HLE::Resolve("libkernel", "ZP4e7rlzOUk#G#H") != 0, "savedata mount3 via libkernel NID");
    EXPECT(HLE::Resolve("libkernel", "j3YMu1MVNNo#U#U") != 0, "user init via libkernel NID");
    EXPECT(HLE::Resolve("libkernel", "fPhymKNvK-A#U#U") != 0, "user list via libkernel NID");
    EXPECT(HLE::Resolve("libkernel", "uoUpLGNkygk#O#P") != 0, "common dialog via libkernel NID");
    EXPECT(HLE::Resolve("libkernel", "ERKzksauAJA#H#I") != 0, "dialog status via libkernel NID");

    // libScePad functions the game imports module-scoped via libkernel NIDs.
    EXPECT(HLE::Resolve("libkernel", "hv1luiJrqQM#L#M") != 0, "scePadInit via libkernel NID");
    EXPECT(HLE::Resolve("libkernel", "xk0AcarP3V4#L#M") != 0, "scePadOpen via libkernel NID");
    EXPECT(HLE::Resolve("libkernel", "6ncge5+l5Qs#L#M") != 0, "scePadClose via libkernel NID");
    EXPECT(HLE::Resolve("libkernel", "YndgXqQVV7c#L#M") != 0, "scePadReadState via libkernel NID");

    // None of the above should have produced an auto-stub.
    EXPECT_EQ(HLE::GetUnresolvedImportCount(), (u64)0, "no auto-stubs for Part-B symbols");

    // Cross-module plain-name resolution.
    EXPECT(HLE::ResolveAny("sceSaveDataDirNameSearch") != 0, "ResolveAny dir search");
    EXPECT(HLE::ResolveAny("sceSaveDataCreateTransactionResource") != 0, "ResolveAny txn resource");
    EXPECT(HLE::ResolveAny("sceSaveDataUmount2") != 0, "ResolveAny umount2");
    EXPECT(HLE::ResolveAny("sceSaveDataPrepare") != 0, "ResolveAny prepare");
    EXPECT(HLE::ResolveAny("sceSaveDataCommit") != 0, "ResolveAny commit");
    EXPECT(HLE::ResolveAny("sceUserServiceGetInitialUser") != 0, "ResolveAny initial user");
    EXPECT(HLE::ResolveAny("sceAgcGetRegisterDefaults2") != 0, "ResolveAny agc defaults2");
}

void TestResolveAnyBareAndNidNames() {
    std::fprintf(stdout, "[TEST] ResolveAny bare-name and NID->plain-name matching\n");

    HLE::SetStrictImportMode(false);
    ResetCounters();

    // Bare name must match a registered "name#T#T" symbol.
    guest_addr_t via_tag  = HLE::Resolve("libkernel", "strcat#T#T");
    guest_addr_t via_bare = HLE::ResolveAny("strcat");
    EXPECT(via_tag != 0, "strcat#T#T registered");
    EXPECT(via_bare == via_tag, "ResolveAny('strcat') matches registered strcat#T#T");

    // NID form is registered directly too (its own thunk, same handler).
    guest_addr_t via_nid = HLE::ResolveAny("Ls4tzzhimqQ#T#T");
    EXPECT(via_nid != 0, "strcat NID resolves");
    EXPECT(ReadSymbolIdFromThunk(via_nid) != 0, "strcat NID thunk id");

    // NID -> plain-name bridge via the NID database.  The built-in table is
    // tiny, so load a one-entry temp DB to exercise the path deterministically.
    const std::string db_path = "pcsx5_test_nid_db.txt";
    {
        std::ofstream db(db_path, std::ios::binary | std::ios::trunc);
        db << "kiZSXIWd9vg\tlibc\tstrcpy\n";
    }
    EXPECT(Common::LoadNidDatabase(db_path), "temp NID db loads");
    std::remove(db_path.c_str());

    guest_addr_t strcpy_tag = HLE::Resolve("libkernel", "strcpy#T#T");
    EXPECT(strcpy_tag != 0, "strcpy#T#T registered");
    guest_addr_t strcpy_via_nid = HLE::ResolveAny("kiZSXIWd9vg#T#T");
    EXPECT(strcpy_via_nid != 0, "ResolveAny(strcpy NID) resolves");
    EXPECT_EQ(HLE::GetUnresolvedImportCount(), (u64)0, "no auto-stubs during name matching");

    // Whatever thunk the NID resolved to, it must behave like strcpy.
    guest_addr_t sbuf = 0;
    EXPECT(Memory::Map(0, 0x1000, Memory::PROT_READ | Memory::PROT_WRITE, &sbuf) == Memory::Status::Ok,
           "map scratch page for strcpy-via-NID test");
    std::memcpy(reinterpret_cast<void*>(sbuf + 0x20), "hello", 6);
    u64 strcpy_id = ReadSymbolIdFromThunk(strcpy_via_nid);
    u64 sret = HleDispatch(strcpy_id, sbuf, sbuf + 0x20, 0, 0, 0, 0, 0x1235, 0);
    EXPECT(sret == sbuf, "strcpy-via-NID returns dst");
    EXPECT(std::strcmp(reinterpret_cast<const char*>(sbuf), "hello") == 0,
           "strcpy-via-NID copied in guest memory");
    Memory::Unmap(sbuf, 0x1000);

    // libc aliases behave like the real thing on guest memory.
    guest_addr_t buf = 0;
    EXPECT(Memory::Map(0, 0x1000, Memory::PROT_READ | Memory::PROT_WRITE, &buf) == Memory::Status::Ok,
           "map scratch page for strcat test");
    std::memcpy(reinterpret_cast<void*>(buf), "foo", 4);
    std::memcpy(reinterpret_cast<void*>(buf + 0x10), "bar", 4);
    u64 strcat_id = ReadSymbolIdFromThunk(via_tag);
    u64 ret = HleDispatch(strcat_id, buf, buf + 0x10, 0, 0, 0, 0, 0x1234, 0);
    EXPECT(ret == buf, "strcat returns dst");
    EXPECT(std::strcmp(reinterpret_cast<const char*>(buf), "foobar") == 0, "strcat concatenated in guest memory");
    Memory::Unmap(buf, 0x1000);
}

void TestSaveDataMountFillsOutParams() {
    std::fprintf(stdout, "[TEST] sceSaveDataMount3 fills out-params and creates host dir\n");

    HLE::SetStrictImportMode(false);
    ResetCounters();
    HLE::SetSaveDataTitleId("TESTTITLE");

    guest_addr_t buf = 0;
    EXPECT(Memory::Map(0, 0x1000, Memory::PROT_READ | Memory::PROT_WRITE, &buf) == Memory::Status::Ok,
           "map scratch page for mount test");
    std::memset(reinterpret_cast<void*>(buf), 0xAA, 0x100); // poison: mount must overwrite

    u64 mount_id = ReadSymbolIdFromThunk(HLE::Resolve("libkernel", "ZP4e7rlzOUk#G#H"));
    EXPECT(mount_id != 0, "mount3 thunk id");
    u64 rc = HleDispatch(mount_id, buf, 0, 0, 0, 0, 0, 0x2000, 0);
    EXPECT_EQ(rc, (u64)0, "mount3 returns success");

    const char* mount_name = reinterpret_cast<const char*>(buf);
    EXPECT(std::strcmp(mount_name, "/savedata0") == 0, "mount point name is /savedata0");
    // Padding after the 16-char name field must be zeroed, not poison.
    bool padding_zero = true;
    for (int i = 16; i < 32; ++i) {
        if (reinterpret_cast<const u8*>(buf)[i] != 0) padding_zero = false;
    }
    EXPECT(padding_zero, "mount out-param padding is zeroed");

    const std::string dir = HLE::GetSaveDataDir();
    EXPECT(!dir.empty(), "save-data dir reported");
    EXPECT(std::filesystem::exists(dir), "host save-data dir created on disk");

    // GetLoginUserIdList: one user, {1, -1, -1, -1}.
    u64 list_id = ReadSymbolIdFromThunk(HLE::Resolve("libkernel", "fPhymKNvK-A#U#U"));
    std::memset(reinterpret_cast<void*>(buf), 0, 0x100);
    u64 count = HleDispatch(list_id, buf, 0, 0, 0, 0, 0, 0x2001, 0);
    EXPECT_EQ(count, (u64)1, "one logged-in user");
    const s32* ids = reinterpret_cast<const s32*>(buf);
    EXPECT(ids[0] == 1 && ids[1] == -1 && ids[2] == -1 && ids[3] == -1,
           "login user id list = {1, -1, -1, -1}");

    // Dialog status reports FINISHED (3) so polling loops exit.
    u64 status_id = ReadSymbolIdFromThunk(HLE::Resolve("libkernel", "ERKzksauAJA#H#I"));
    u64 status = HleDispatch(status_id, 0, 0, 0, 0, 0, 0, 0x2002, 0);
    EXPECT_EQ(status, (u64)3, "sceSaveDataDialogGetStatus -> FINISHED");

    Memory::Unmap(buf, 0x1000);

    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(dir).parent_path(), ec);
}

void TestGetptolowerReturnsTable() {
    std::fprintf(stdout, "[TEST] _Getptolower returns a persistent tolower table\n");

    HLE::SetStrictImportMode(false);
    ResetCounters();

    // The game imports the raw NID cross-module; #j#j variant must match the
    // #T#T registration via base-NID matching.
    guest_addr_t thunk   = HLE::ResolveAny("1uJgoVq3bQU#T#T");
    guest_addr_t thunk_j = HLE::Resolve("libkernel", "1uJgoVq3bQU#j#j");
    EXPECT(thunk != 0, "_Getptolower NID resolves");
    EXPECT(thunk_j != 0, "_Getptolower #j#j variant resolves");
    EXPECT_EQ(HLE::GetUnresolvedImportCount(), (u64)0, "no auto-stub for _Getptolower");

    u64 id = ReadSymbolIdFromThunk(thunk);
    u64 table1 = HleDispatch(id, 0, 0, 0, 0, 0, 0, 0x3000, 0);
    u64 table2 = HleDispatch(id, 0, 0, 0, 0, 0, 0, 0x3001, 0);
    EXPECT(table1 != 0, "_Getptolower returns non-null table pointer");
    EXPECT(table1 == table2, "_Getptolower returns a persistent table");

    const u16* tab = reinterpret_cast<const u16*>(table1);
    EXPECT(tab['A'] == 'a' && tab['Z'] == 'z', "table maps A-Z to a-z");
    EXPECT(tab['0'] == '0' && tab['a'] == 'a' && tab[0] == 0,
           "non-uppercase entries map to themselves");
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    if (!Memory::Initialize()) {
        std::fprintf(stderr, "FATAL: Memory::Initialize failed\n");
        return 2;
    }
    if (!HLE::Initialize()) {
        std::fprintf(stderr, "FATAL: HLE::Initialize failed\n");
        Memory::Shutdown();
        return 2;
    }

    TestResolveAndReport();
    TestJsonExport();
    TestStrictModeFailsLoudly();
    TestNewModulesResolve();
    TestResolveAnyBareAndNidNames();
    TestSaveDataMountFillsOutParams();
    TestGetptolowerReturnsTable();

    HLE::Shutdown();
    Memory::Shutdown();

    if (g_failures == 0) {
        std::fprintf(stdout, "HLE import-report tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "HLE import-report tests: %d failure(s).\n", g_failures);
    return 1;
}
