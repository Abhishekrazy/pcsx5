// PS5 sce_sys/param.json parser tests.
//
// Verifies:
//   - Full parse of a representative document (modeled on the real
//     PPSA02929 / PPSA10112 dumps under Games/*/sce_sys/).
//   - Missing optional fields keep defaults and are not fatal.
//   - Missing titleId is fatal.
//   - Malformed JSON is rejected.
//   - Real dump file parses when present (skipped gracefully otherwise).
//
// Self-contained: no Memory / Kernel / HLE dependencies.

#include "loader/param_json.h"

#include "common/log.h"
#include "common/types.h"

#include <cstdio>
#include <filesystem>
#include <string>

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

#define EXPECT_EQ(a, b, msg) EXPECT((a) == (b), (msg))

// Representative document trimmed down from the real PPSA02929 dump.
const char* kFullDoc = R"json({
    "addcont": {
        "serviceIdForSharing": [
            "                   ",
            "                   "
        ]
    },
    "ageLevel": {
        "JP": 15,
        "US": 13,
        "default": 16
    },
    "applicationCategoryType": 0,
    "applicationDrmType": "standard",
    "attribute": 536870912,
    "attribute2": 0,
    "attribute3": 0,
    "conceptId": "10000999",
    "contentBadgeType": 1,
    "contentId": "UP0891-PPSA02929_00-RAGDREAMINGSARAH",
    "contentVersion": "01.000.000",
    "localizedParameters": {
        "defaultLanguage": "en-US",
        "en-US": { "titleName": "Dreaming Sarah" },
        "ja-JP": { "titleName": "Dreaming Sarah" }
    },
    "masterVersion": "01.01",
    "requiredSystemSoftwareVersion": "0x0200000000000000",
    "sdkVersion": "0x0200000000000000",
    "titleId": "PPSA02929",
    "userDefinedParam1": 3,
    "userDefinedParam2": 0,
    "userDefinedParam3": 0,
    "userDefinedParam4": 0
})json";

void TestFullParse() {
    Loader::ParamJson p;
    EXPECT(Loader::ParseParamJsonString(kFullDoc, p), "full document parse failed");

    EXPECT_EQ(p.titleId, std::string("PPSA02929"), "titleId");
    EXPECT_EQ(p.contentId, std::string("UP0891-PPSA02929_00-RAGDREAMINGSARAH"),
              "contentId");
    EXPECT_EQ(p.conceptId, std::string("10000999"), "conceptId");
    EXPECT_EQ(p.contentVersion, std::string("01.000.000"), "contentVersion");
    EXPECT_EQ(p.masterVersion, std::string("01.01"), "masterVersion");
    EXPECT_EQ(p.requiredSystemSoftwareVersion,
              std::string("0x0200000000000000"), "requiredSystemSoftwareVersion");
    EXPECT_EQ(p.sdkVersion, std::string("0x0200000000000000"), "sdkVersion");
    EXPECT_EQ(p.applicationDrmType, std::string("standard"), "applicationDrmType");

    EXPECT_EQ(p.applicationCategoryType, 0u, "applicationCategoryType");
    EXPECT_EQ(p.contentBadgeType, 1u, "contentBadgeType");
    EXPECT_EQ(p.attribute, 536870912u, "attribute");
    EXPECT_EQ(p.attribute2, 0u, "attribute2");
    EXPECT_EQ(p.attribute3, 0u, "attribute3");
    EXPECT_EQ(p.userDefinedParam1, 3u, "userDefinedParam1");
    EXPECT_EQ(p.userDefinedParam2, 0u, "userDefinedParam2");
    EXPECT_EQ(p.userDefinedParam3, 0u, "userDefinedParam3");
    EXPECT_EQ(p.userDefinedParam4, 0u, "userDefinedParam4");

    EXPECT_EQ(p.addcontServiceIds.size(), size_t(2), "addcont service id count");
    EXPECT_EQ(p.ageLevel.size(), size_t(3), "ageLevel count");
    EXPECT_EQ(p.ageLevel.at("US"), 13u, "ageLevel[US]");
    EXPECT_EQ(p.ageLevel.at("default"), 16u, "ageLevel[default]");

    EXPECT_EQ(p.defaultLanguage, std::string("en-US"), "defaultLanguage");
    EXPECT_EQ(p.titleNames.size(), size_t(2), "titleNames count");
    EXPECT_EQ(p.titleNames.at("en-US"), std::string("Dreaming Sarah"),
              "titleNames[en-US]");
}

void TestMissingOptionalFields() {
    Loader::ParamJson p;
    EXPECT(Loader::ParseParamJsonString(R"({"titleId": "PPSA00001"})", p),
           "minimal document parse failed");
    EXPECT_EQ(p.titleId, std::string("PPSA00001"), "titleId");
    EXPECT(p.contentId.empty(), "contentId defaults to empty");
    EXPECT(p.masterVersion.empty(), "masterVersion defaults to empty");
    EXPECT(p.requiredSystemSoftwareVersion.empty(),
           "requiredSystemSoftwareVersion defaults to empty");
    EXPECT_EQ(p.applicationCategoryType, 0u, "category defaults to 0");
    EXPECT_EQ(p.attribute, 0u, "attribute defaults to 0");
    EXPECT(p.addcontServiceIds.empty(), "no addcont ids by default");
    EXPECT(p.ageLevel.empty(), "no age levels by default");
    EXPECT(p.titleNames.empty(), "no title names by default");
}

void TestMissingTitleId() {
    Loader::ParamJson p;
    EXPECT(!Loader::ParseParamJsonString(R"({"contentId": "X"})", p),
           "missing titleId must fail");
    EXPECT(!Loader::ParseParamJsonString(R"({"titleId": ""})", p),
           "empty titleId must fail");
}

void TestMalformedJson() {
    Loader::ParamJson p;
    EXPECT(!Loader::ParseParamJsonString("{ not json", p),
           "malformed JSON must fail");
    EXPECT(!Loader::ParseParamJsonString("", p), "empty input must fail");
    EXPECT(!Loader::ParseParamJsonString("[]", p), "non-object root must fail");
}

// Real dump files, skipped gracefully when absent (like dump_imports).
void TestRealDumps() {
    constexpr const char* kDumps[] = {
        "I:/Personal/Windows/pcsx5/Games/PPSA02929-app0/sce_sys/param.json",
        "I:/Personal/Windows/pcsx5/Games/PPSA10112-app/sce_sys/param.json",
    };
    int found = 0;
    for (const char* path : kDumps) {
        if (!std::filesystem::exists(path)) {
            std::fprintf(stderr, "[SKIP] %s not present on this host.\n", path);
            continue;
        }
        ++found;
        Loader::ParamJson p;
        EXPECT(Loader::ParseParamJson(path, p), "real dump parse failed");
        if (!p.titleId.empty()) {
            EXPECT_EQ(p.titleId.size(), size_t(9), "real titleId length");
            EXPECT_EQ(p.titleId.rfind("PPSA", 0), size_t(0), "real titleId prefix");
        }
        EXPECT(!p.contentVersion.empty(), "real contentVersion present");
        EXPECT(!p.requiredSystemSoftwareVersion.empty(),
               "real requiredSystemSoftwareVersion present");
    }
    if (found == 0) {
        std::fprintf(stderr, "[SKIP] no real dumps available.\n");
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    LogConfig::SetLevel(LogCategory::Loader, LogLevel::Warn);

    std::fprintf(stdout, "=== param_json_tests ===\n");

    TestFullParse();
    TestMissingOptionalFields();
    TestMissingTitleId();
    TestMalformedJson();
    TestRealDumps();

    std::fprintf(stdout, "  %d/%d checks passed\n",
                 g_checks - g_failures, g_checks);
    if (g_failures != 0) {
        std::fprintf(stderr, "  %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stdout, "OK\n");
    return 0;
}
