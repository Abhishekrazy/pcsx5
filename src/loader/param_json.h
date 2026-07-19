// PS5 sce_sys/param.json parser.
//
// PS4 dumps ship param.sfo (binary); PS5 dumps replace it with a JSON
// document describing the title, version, category, and publishing
// attributes.  This parser is deliberately tolerant: unknown fields are
// ignored, missing optional fields keep their defaults, and only a
// malformed document or a missing titleId makes parsing fail.
//
// Schema informed by real dumps under Games/*/sce_sys/param.json
// (PPSA02929, PPSA10112, PPSA20591, PPSA23885, PPSA01668).

#pragma once

#include "../common/types.h"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace Loader {

struct ParamJson {
    // Identification (all real dumps carry these).
    std::string titleId;          // "PPSA02929" — required.
    std::string contentId;        // "UP0891-PPSA02929_00-..."
    std::string conceptId;        // numeric string, e.g. "10000999"

    // Versioning.
    std::string contentVersion;   // "01.000.000" (application version)
    std::string masterVersion;    // "01.01"
    std::string requiredSystemSoftwareVersion;  // "0x0200000000000000"
    std::string sdkVersion;       // "0x0500000000000000"

    // Category / DRM.
    u32 applicationCategoryType = 0;  // 0 = game, other values for addcont etc.
    std::string applicationDrmType;   // "standard"
    u32 contentBadgeType = 0;

    // Publishing attributes and user-defined scratch values.
    u32 attribute  = 0;
    u32 attribute2 = 0;
    u32 attribute3 = 0;
    u32 userDefinedParam1 = 0;
    u32 userDefinedParam2 = 0;
    u32 userDefinedParam3 = 0;
    u32 userDefinedParam4 = 0;

    // Add-on content service labels (fixed 7 slots of 19-char ids).
    std::vector<std::string> addcontServiceIds;

    // Per-country age ratings (country code -> level, e.g. "US" -> 17).
    std::map<std::string, u32> ageLevel;

    // localizedParameters.defaultLanguage plus per-language title names.
    std::string defaultLanguage;
    std::map<std::string, std::string> titleNames;
};

// Parse from file contents (already loaded).  Returns false on malformed
// JSON or a missing/empty titleId; a warning is logged in both cases.
bool ParseParamJsonString(std::string_view content, ParamJson& out);

// Load and parse a param.json file.  Returns false if the file cannot be
// read, the JSON is malformed, or titleId is missing/empty.
bool ParseParamJson(const std::filesystem::path& file, ParamJson& out);

}  // namespace Loader
