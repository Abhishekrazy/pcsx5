// Config service tests — round-trip, missing file, corrupt file, defaults,
// migration, per-title override layering.

#include "config/config.h"
#include "common/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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

// Per-process scratch directory under the system temp dir.  Using a stable
// absolute path keeps the tests deterministic regardless of the cwd chosen by
// the test runner (e.g. ctest vs. direct invocation).
std::string ScratchDir(const std::string& leaf) {
    auto base = std::filesystem::temp_directory_path() / "pcsx5_config_test" / leaf;
    return base.string();
}

void WriteRaw(const std::filesystem::path& p, const std::string& body) {
    std::ofstream f(p, std::ios::trunc | std::ios::binary);
    f << body;
}

std::string Slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}

// ---------------------------------------------------------------------------
// 1. Defaults() matches the documented baseline
// ---------------------------------------------------------------------------
void TestDefaults() {
    std::fprintf(stdout, "[TEST] Config defaults\n");
    auto d = ConfigService::Config::Defaults();
    EXPECT_EQ(static_cast<int>(d.logging.min_level), static_cast<int>(LogLevel::Info),
              "default log level is Info");
    EXPECT(!d.logging.json_output, "JSON output off by default");
    EXPECT(d.logging.file_path.empty(), "no log file by default");
    EXPECT_STR_EQ(d.crash.bundle_dir, std::string("pcsx5_crash"), "default crash dir");
    EXPECT(d.crash.write_minidump, "minidump on by default");
    EXPECT(!d.hle.strict_imports, "strict imports off by default");
    EXPECT(d.hle.trace_calls, "trace calls on by default");
    EXPECT_EQ(d.hle.trace_capacity, 256, "trace capacity default 256");
    EXPECT_EQ(d.graphics.width,  1280, "default width");
    EXPECT_EQ(d.graphics.height, 720,  "default height");
    EXPECT(!d.graphics.fullscreen, "windowed by default");
    EXPECT_EQ(d.graphics.renderer, 0, "Vulkan by default");
    EXPECT_EQ(d.audio.backend, 0, "audio off by default");
    EXPECT_EQ(d.input.backend, 0, "SDL input by default");
    EXPECT(!d.loaded_from_disk, "defaults are not 'loaded from disk'");
}

// ---------------------------------------------------------------------------
// 2. Save then load round-trip preserves every field
// ---------------------------------------------------------------------------
void TestRoundTrip() {
    std::fprintf(stdout, "[TEST] Config save+load round-trip\n");
    const std::string dir = ScratchDir("rt");
    std::filesystem::remove_all(dir);
    ConfigService::Initialize(dir);

    auto& g = ConfigService::MutableGlobal();
    g.logging.min_level   = LogLevel::Debug;
    g.logging.json_output = true;
    g.logging.file_path   = "rotated.log";
    g.logging.file_append = true;
    g.crash.bundle_dir    = "rt_crash";
    g.crash.write_minidump= false;
    g.hle.strict_imports  = true;
    g.hle.trace_capacity  = 512;
    g.graphics.width      = 1920;
    g.graphics.height     = 1080;
    g.graphics.fullscreen = true;
    g.graphics.resolution_scale = 1.5f;
    g.audio.backend       = 1;
    g.audio.buffer_ms     = 25;
    g.audio.volume        = 0.75f;
    g.input.backend       = 1;
    g.input.deadzone      = 0.25f;
    g.input.rumble        = false;
    EXPECT(ConfigService::SaveGlobal(), "SaveGlobal succeeds");

    // Re-init in a new service instance to force a fresh load
    ConfigService::Initialize(dir);
    const auto& g2 = ConfigService::Global();
    EXPECT(g2.loaded_from_disk, "config came from disk");
    EXPECT_EQ(g2.schema_version, ConfigService::kCurrentSchemaVersion, "schema version");
    EXPECT_EQ(static_cast<int>(g2.logging.min_level), static_cast<int>(LogLevel::Debug),
              "log level preserved");
    EXPECT(g2.logging.json_output, "json output preserved");
    EXPECT_STR_EQ(g2.logging.file_path, std::string("rotated.log"), "log file preserved");
    EXPECT(g2.logging.file_append, "log append preserved");
    EXPECT_STR_EQ(g2.crash.bundle_dir, std::string("rt_crash"), "crash dir preserved");
    EXPECT(!g2.crash.write_minidump, "minidump flag preserved");
    EXPECT(g2.hle.strict_imports, "strict imports preserved");
    EXPECT_EQ(g2.hle.trace_capacity, 512, "trace capacity preserved");
    EXPECT_EQ(g2.graphics.width, 1920, "graphics width preserved");
    EXPECT(g2.graphics.fullscreen, "fullscreen preserved");
    EXPECT_EQ(g2.audio.backend, 1, "audio backend preserved");
    EXPECT_EQ(g2.input.backend, 1, "input backend preserved");
    EXPECT(g2.input.deadzone > 0.24f && g2.input.deadzone < 0.26f, "deadzone preserved");

    // Make sure the file actually contains a schema_version field
    const std::string body = Slurp(dir + "/global.json");
    EXPECT(body.find("\"schema_version\"") != std::string::npos, "schema_version in file");
    EXPECT(body.find("\"min_level\"") != std::string::npos, "logging.min_level in file");
}

// ---------------------------------------------------------------------------
// 3. Missing global.json -> defaults are written and returned
// ---------------------------------------------------------------------------
void TestMissingFile() {
    std::fprintf(stdout, "[TEST] Missing config file -> defaults\n");
    const std::string dir = ScratchDir("missing");
    std::filesystem::remove_all(dir);

    ConfigService::Initialize(dir);
    EXPECT(std::filesystem::exists(dir + "/global.json"),
           "defaults written when global.json is absent");
    const auto& g = ConfigService::Global();
    EXPECT(g.loaded_from_disk, "loaded from disk after auto-write");
    EXPECT_EQ(static_cast<int>(g.logging.min_level), static_cast<int>(LogLevel::Info),
              "default log level on fresh init");
}

// ---------------------------------------------------------------------------
// 4. Corrupt global.json -> defaults are used and a warning is logged
// ---------------------------------------------------------------------------
void TestCorruptFile() {
    std::fprintf(stdout, "[TEST] Corrupt config file -> fall back to defaults\n");
    const std::string dir = ScratchDir("corrupt");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    WriteRaw(dir + "/global.json", "{ this is not valid JSON ###");

    ConfigService::Initialize(dir);
    const auto& g = ConfigService::Global();
    // Either it failed to parse and we kept defaults (loaded_from_disk=false),
    // or we somehow re-wrote defaults.  Both paths should expose a usable
    // object that does not crash and is internally consistent.
    EXPECT_EQ(static_cast<int>(g.logging.min_level), static_cast<int>(LogLevel::Info),
              "fall-back to default log level");
    EXPECT_STR_EQ(g.crash.bundle_dir, std::string("pcsx5_crash"),
                  "fall-back to default crash dir");
}

// ---------------------------------------------------------------------------
// 5. Per-title override layers on top of global
// ---------------------------------------------------------------------------
void TestPerTitleOverride() {
    std::fprintf(stdout, "[TEST] Per-title override layering\n");
    const std::string dir = ScratchDir("title");
    std::filesystem::remove_all(dir);
    ConfigService::Initialize(dir);

    // Global: small window, windowed
    auto& g = ConfigService::MutableGlobal();
    g.graphics.width = 1280;
    g.graphics.height = 720;
    g.graphics.fullscreen = false;
    g.hle.strict_imports = false;
    EXPECT(ConfigService::SaveGlobal(), "save global");

    // Title: fullscreen 4K
    const std::string title = "CUSA00001";
    auto& t = ConfigService::MutableForTitle(title);
    t.graphics.width = 3840;
    t.graphics.height = 2160;
    t.graphics.fullscreen = true;
    t.hle.strict_imports = true;
    EXPECT(ConfigService::SavePerTitle(title), "save per-title");

    // Re-init to force a fresh load from disk
    ConfigService::Initialize(dir);

    // Global query ignores per-title
    const auto& gq = ConfigService::Global();
    EXPECT_EQ(gq.graphics.width, 1280, "global width unchanged");
    EXPECT(!gq.hle.strict_imports, "global strict off");

    // Effective view applies the per-title overlay
    auto eff = ConfigService::EffectiveFor(title);
    EXPECT_EQ(eff.graphics.width, 3840, "title width applied");
    EXPECT_EQ(eff.graphics.height, 2160, "title height applied");
    EXPECT(eff.graphics.fullscreen, "title fullscreen applied");
    EXPECT(eff.hle.strict_imports, "title strict applied");

    // Empty title id -> global view
    auto empty = ConfigService::EffectiveFor("");
    EXPECT_EQ(empty.graphics.width, 1280, "empty title id -> global");
    EXPECT(!empty.hle.strict_imports, "empty title id -> global strict off");

    // Per-title file should be on disk
    EXPECT(std::filesystem::exists(dir + "/titles/" + title + ".json"),
           "per-title file written");
}

// ---------------------------------------------------------------------------
// 6. Unknown future schema passes through with a warning
// ---------------------------------------------------------------------------
void TestForwardCompat() {
    std::fprintf(stdout, "[TEST] Future schema version passes through\n");
    const std::string dir = ScratchDir("future");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    WriteRaw(dir + "/global.json",
             "{\n"
             "  \"schema_version\": 999,\n"
             "  \"logging\": { \"min_level\": \"Debug\" }\n"
             "}\n");

    ConfigService::Initialize(dir);
    const auto& g = ConfigService::Global();
    // Should still parse logging fields even if version is unknown
    EXPECT_EQ(static_cast<int>(g.logging.min_level), static_cast<int>(LogLevel::Debug),
              "forward-compat preserves readable fields");
    EXPECT_EQ(g.schema_version, ConfigService::kCurrentSchemaVersion,
              "schema coerced to current after migration pass-through");
}

} // namespace

// ---------------------------------------------------------------------------
// 7. Savedata crypto keys survive a per-title save/load round-trip
// ---------------------------------------------------------------------------
void TestSaveDataCryptoRoundTrip() {
    std::fprintf(stdout, "[TEST] Savedata crypto keys round-trip\n");
    const std::string dir = ScratchDir("sdcrypto");
    std::filesystem::remove_all(dir);
    ConfigService::Initialize(dir);

    const std::string title = "PPSA99999";
    ConfigService::SaveDataCrypto keys;
    keys.enabled = true;
    for (size_t i = 0; i < 16; ++i) {
        keys.xts_key1[i] = static_cast<std::uint8_t>(i);
        keys.xts_key2[i] = static_cast<std::uint8_t>(0xF0 + i);
    }
    {
        auto& t = ConfigService::MutableForTitle(title);
        t.savedata_crypto = keys;
        EXPECT(ConfigService::SavePerTitle(title), "save per-title with crypto keys");
    }

    // Hex strings should be visible in the on-disk file.
    const std::string body = Slurp(dir + "/titles/" + title + ".json");
    EXPECT(body.find("\"savedata_crypto\"") != std::string::npos, "savedata_crypto in file");
    EXPECT(body.find("000102030405060708090a0b0c0d0e0f") != std::string::npos,
           "xts_key1 serialised as hex");

    ConfigService::Initialize(dir); // fresh load
    auto loaded = ConfigService::SaveDataKeysFor(title);
    EXPECT(loaded.has_value(), "SaveDataKeysFor returns keys for enabled title");
    if (loaded) {
        EXPECT(loaded->enabled, "keys enabled after reload");
        EXPECT(loaded->xts_key1 == keys.xts_key1, "xts_key1 preserved");
        EXPECT(loaded->xts_key2 == keys.xts_key2, "xts_key2 preserved");
    }

    // A title with no savedata_crypto (disabled default) yields nullopt.
    EXPECT(!ConfigService::SaveDataKeysFor("CUSA_NOKEYS").has_value(),
           "disabled keys -> nullopt");
}

// ---------------------------------------------------------------------------
// 8. Malformed savedata_crypto hex keys are coerced to disabled
// ---------------------------------------------------------------------------
void TestSaveDataCryptoMalformed() {
    std::fprintf(stdout, "[TEST] Malformed savedata crypto keys -> disabled\n");
    const std::string dir = ScratchDir("sdcrypto_bad");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir + "/titles");
    WriteRaw(dir + "/global.json",
             "{\n  \"schema_version\": 3\n}\n");
    WriteRaw(dir + "/titles/BADKEYS.json",
             "{\n"
             "  \"schema_version\": 3,\n"
             "  \"savedata_crypto\": {\n"
             "    \"enabled\": true,\n"
             "    \"xts_key1\": \"zzzz_not_hex_zzzz_not_hex_zzzz\",\n"
             "    \"xts_key2\": \"0011\"\n"
             "  }\n"
             "}\n");

    ConfigService::Initialize(dir);
    auto keys = ConfigService::SaveDataKeysFor("BADKEYS");
    EXPECT(!keys.has_value(), "malformed hex keys -> treated as disabled");
    const ConfigService::Config* t = ConfigService::ForTitle("BADKEYS");
    EXPECT(t != nullptr, "per-title record still loaded");
    if (t) EXPECT(!t->savedata_crypto.enabled, "enabled flag cleared on load");
}

// ---------------------------------------------------------------------------
// 9. Multi-profile accessors: active profile, lookup by id, active id
// ---------------------------------------------------------------------------
void TestMultiProfileAccessors() {
    std::fprintf(stdout, "[TEST] Multi-profile accessors\n");
    const std::string dir = ScratchDir("users");
    std::filesystem::remove_all(dir);
    ConfigService::Initialize(dir);

    auto& g = ConfigService::MutableGlobal();
    g.users.profiles.clear();
    ConfigService::UserProfile p1;
    p1.id = ConfigService::kFirstUserId; p1.name = "Alice"; p1.online_id = "alice_psn";
    ConfigService::UserProfile p2;
    p2.id = ConfigService::kFirstUserId + 1; p2.name = "Bob"; p2.online_id = "bob_psn";
    g.users.profiles.push_back(p1);
    g.users.profiles.push_back(p2);
    g.users.active_user = 1;
    EXPECT(ConfigService::SaveGlobal(), "save multi-profile global");

    ConfigService::Initialize(dir); // fresh load

    const auto* active = ConfigService::ActiveUserProfile();
    EXPECT(active != nullptr, "active profile exists");
    if (active) {
        EXPECT_EQ(active->id, ConfigService::kFirstUserId + 1, "active profile id");
        EXPECT_STR_EQ(active->name, std::string("Bob"), "active profile name");
        EXPECT_STR_EQ(active->online_id, std::string("bob_psn"), "active online id");
    }
    EXPECT_EQ(ConfigService::ActiveUserId(), ConfigService::kFirstUserId + 1,
              "ActiveUserId matches active profile");
    const auto* found = ConfigService::FindUserProfile(ConfigService::kFirstUserId);
    EXPECT(found != nullptr, "FindUserProfile finds first profile");
    if (found) EXPECT_STR_EQ(found->name, std::string("Alice"), "found profile name");
    EXPECT(ConfigService::FindUserProfile(0xDEADBEEF) == nullptr,
           "FindUserProfile unknown id -> nullptr");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    TestDefaults();
    TestRoundTrip();
    TestMissingFile();
    TestCorruptFile();
    TestPerTitleOverride();
    TestForwardCompat();
    TestSaveDataCryptoRoundTrip();
    TestSaveDataCryptoMalformed();
    TestMultiProfileAccessors();

    std::fprintf(stdout, "Config: %d check(s), %d failure(s)\n", g_checks, g_failures);
    // Always clean up the scratch directories so a previous failing run
    // cannot leak state into the next invocation.
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::temp_directory_path() / "pcsx5_config_test", ec);
    if (g_failures == 0) {
        std::fprintf(stdout, "Config tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "Config tests FAILED with %d failure(s).\n", g_failures);
    return 1;
}
