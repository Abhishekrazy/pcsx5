#include "param_json.h"

#include "../common/log.h"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace Loader {
namespace {

using nlohmann::json;

// Tolerant scalar readers: silently keep the default when the key is
// missing or has an unexpected type.

void ReadString(const json& j, const char* key, std::string& out) {
    auto it = j.find(key);
    if (it != j.end() && it->is_string()) {
        out = it->get<std::string>();
    }
}

void ReadU32(const json& j, const char* key, u32& out) {
    auto it = j.find(key);
    if (it != j.end() && it->is_number_unsigned()) {
        out = it->get<u32>();
    } else if (it != j.end() && it->is_number_integer()) {
        out = static_cast<u32>(it->get<s64>());
    }
}

}  // namespace

bool ParseParamJsonString(std::string_view content, ParamJson& out) {
    json j = json::parse(content, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        LOG_ERROR(Loader, "param.json: malformed JSON document");
        return false;
    }

    out = ParamJson{};

    ReadString(j, "titleId", out.titleId);
    ReadString(j, "contentId", out.contentId);
    ReadString(j, "conceptId", out.conceptId);
    ReadString(j, "contentVersion", out.contentVersion);
    ReadString(j, "masterVersion", out.masterVersion);
    ReadString(j, "requiredSystemSoftwareVersion", out.requiredSystemSoftwareVersion);
    ReadString(j, "sdkVersion", out.sdkVersion);
    ReadString(j, "applicationDrmType", out.applicationDrmType);

    ReadU32(j, "applicationCategoryType", out.applicationCategoryType);
    ReadU32(j, "contentBadgeType", out.contentBadgeType);
    ReadU32(j, "attribute", out.attribute);
    ReadU32(j, "attribute2", out.attribute2);
    ReadU32(j, "attribute3", out.attribute3);
    ReadU32(j, "userDefinedParam1", out.userDefinedParam1);
    ReadU32(j, "userDefinedParam2", out.userDefinedParam2);
    ReadU32(j, "userDefinedParam3", out.userDefinedParam3);
    ReadU32(j, "userDefinedParam4", out.userDefinedParam4);

    // addcont.serviceIdForSharing: array of fixed-width service labels.
    auto ac = j.find("addcont");
    if (ac != j.end() && ac->is_object()) {
        auto ids = ac->find("serviceIdForSharing");
        if (ids != ac->end() && ids->is_array()) {
            for (const auto& id : *ids) {
                if (id.is_string()) {
                    out.addcontServiceIds.push_back(id.get<std::string>());
                }
            }
        }
    }

    // ageLevel: map of country code -> numeric level ("default" included).
    auto al = j.find("ageLevel");
    if (al != j.end() && al->is_object()) {
        for (auto it = al->begin(); it != al->end(); ++it) {
            if (it.value().is_number()) {
                out.ageLevel[it.key()] = it.value().get<u32>();
            }
        }
    }

    // localizedParameters: defaultLanguage + per-language titleName.
    auto lp = j.find("localizedParameters");
    if (lp != j.end() && lp->is_object()) {
        ReadString(*lp, "defaultLanguage", out.defaultLanguage);
        for (auto it = lp->begin(); it != lp->end(); ++it) {
            if (it.key() == "defaultLanguage" || !it.value().is_object()) {
                continue;
            }
            std::string name;
            ReadString(it.value(), "titleName", name);
            if (!name.empty()) {
                out.titleNames[it.key()] = name;
            }
        }
    }

    if (out.titleId.empty()) {
        LOG_ERROR(Loader, "param.json: missing or empty titleId");
        return false;
    }

    LOG_INFO(Loader,
             "param.json: titleId=%s contentVersion=%s masterVersion=%s category=%u",
             out.titleId.c_str(), out.contentVersion.c_str(),
             out.masterVersion.c_str(), out.applicationCategoryType);
    return true;
}

bool ParseParamJson(const std::filesystem::path& file, ParamJson& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        LOG_ERROR(Loader, "param.json: cannot open '%s'",
                  file.string().c_str());
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ParseParamJsonString(ss.str(), out);
}

}  // namespace Loader
