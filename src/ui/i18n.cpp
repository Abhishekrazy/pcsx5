// i18n.cpp — Translation / localisation system for pcsx5_ui.
//
// All user-visible strings are keyed by an ASCII identifier and resolved
// through I18n::Tr(key).  Translations are loaded from JSON files in
// assets/lang/ at runtime.  Currently only en-US is fully populated; other
// languages fall back to en-US for every key.  Adding a new language is a
// matter of creating a new JSON file in assets/lang/.
//
// Keys follow a loose convention:
//   section.element        e.g. "sidebar.emulator"
//   section.element.detail e.g. "status.running"
//   dialog.title           e.g. "about.title"
//   action.label           e.g. "button.play"

#include "i18n.h"
#include <cstring>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace Ui {
namespace I18n {

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
static Language g_active_language = Language::EnUS;
static bool     g_initialized     = false;

// Translation tables — unordered_map<const char*, const char*> per language.
// We use const char* keys so they can be compared with strcmp in a custom
// hash/eq functor, avoiding std::string allocations for every lookup.
struct CStrHash {
    std::size_t operator()(const char* s) const noexcept {
        std::size_t h = 0;
        if (!s) return h;
        // FNV-1a 64-bit
        while (*s) { h ^= static_cast<std::size_t>(*s++); h *= 1099511628211ULL; }
        return h;
    }
};
struct CStrEq {
    bool operator()(const char* a, const char* b) const noexcept {
        return a == b || (a && b && std::strcmp(a, b) == 0);
    }
};

using Table = std::unordered_map<const char*, const char*, CStrHash, CStrEq>;

static Table g_tables[static_cast<int>(Language::Count)];

// ---------------------------------------------------------------------------
// JSON file loading utilities
// ---------------------------------------------------------------------------
static std::string GetLangDir() {
    // Try to find assets/lang relative to the executable
    std::filesystem::path exe_path = std::filesystem::current_path();
    // Check common locations
    std::vector<std::filesystem::path> candidates = {
        exe_path / "assets" / "lang",
        exe_path / ".." / "assets" / "lang",
        exe_path / ".." / ".." / "assets" / "lang",
        "assets/lang",
        "../assets/lang",
        "../../assets/lang"
    };
    
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate) && std::filesystem::is_directory(candidate)) {
            return std::filesystem::absolute(candidate).string();
        }
    }
    return "assets/lang"; // fallback
}

static std::string LanguageToFilename(Language lang) {
    switch (lang) {
        case Language::EnUS: return "en-US.json";
        case Language::JaJP: return "ja-JP.json";
        case Language::KoKR: return "ko-KR.json";
        case Language::ZhCN: return "zh-CN.json";
        case Language::ZhTW: return "zh-TW.json";
        case Language::FrFR: return "fr-FR.json";
        case Language::DeDE: return "de-DE.json";
        case Language::EsES: return "es-ES.json";
        case Language::PtBR: return "pt-BR.json";
        case Language::ItIT: return "it-IT.json";
        case Language::RuRU: return "ru-RU.json";
        default: return "en-US.json";
    }
}

static bool LoadJsonTable(Language lang, Table& table) {
    std::string lang_dir = GetLangDir();
    std::string filename = LanguageToFilename(lang);
    std::filesystem::path filepath = std::filesystem::path(lang_dir) / filename;
    
    if (!std::filesystem::exists(filepath)) {
        return false;
    }
    
    try {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            return false;
        }
        
        nlohmann::json json;
        file >> json;
        
        if (!json.is_object()) {
            return false;
        }
        
        for (auto& [key, value] : json.items()) {
            if (value.is_string()) {
                // Store the string in the table - we need to keep the string alive
                // We'll use strdup to allocate persistent storage
#if defined(_WIN32)
                const char* key_str = _strdup(key.c_str());
                const char* val_str = _strdup(value.get<std::string>().c_str());
#else
                const char* key_str = strdup(key.c_str());
                const char* val_str = strdup(value.get<std::string>().c_str());
#endif
                table[key_str] = val_str;
            }
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

static void FreeTable(Table& table) {
    for (auto& [key, value] : table) {
        free(const_cast<char*>(key));
        free(const_cast<char*>(value));
    }
    table.clear();
}

// ---------------------------------------------------------------------------
// Populate all tables.  Load from JSON files; fall back to hardcoded en-US
// if JSON loading fails.
// ---------------------------------------------------------------------------
void Init() {
    if (g_initialized) return;

    // -- en-US (canonical) ------------------------------------------------
    Table& en = g_tables[static_cast<int>(Language::EnUS)];
    
    // Try to load from JSON first
    if (!LoadJsonTable(Language::EnUS, en)) {
        // Fallback to hardcoded values
        // Sidebar section headers
        en["sidebar.emulator"]   = "EMULATOR";
        en["sidebar.tools"]      = "TOOLS";
        en["sidebar.audio"]      = "AUDIO";
        en["sidebar.view"]       = "VIEW";
        en["sidebar.help"]       = "HELP";
        en["sidebar.language"]   = "Language";

        // Sidebar status pills
        en["status.running"]     = "RUNNING";
        en["status.paused"]      = "PAUSED";
        en["status.idle"]        = "IDLE";

        // Sidebar backend labels
        en["backend.ready"]      = "Backend ready";
        en["backend.not_found"]  = "Backend not found";

        // Sidebar buttons
        en["button.refresh_library"] = "Refresh library";
        en["button.refresh_status"]  = "Refresh status";
        en["button.about"]           = "About pcsx5";

        // Sidebar audio toggle
        en["audio.title_music"]  = "Title music";

        // Sidebar view tabs
        en["view.library"]       = "Library";
        en["view.system"]        = "System";

        // Sidebar console checkbox
        en["sidebar.console"]    = "Console";

        // Top bar
        en["topbar.title"]       = "pcsx5";
        en["topbar.subtitle"]    = "PlayStation 5 HLE Emulator";
        en["topbar.backend_ready"]    = "BACKEND: READY";
        en["topbar.backend_not_found"]= "BACKEND: NOT FOUND";

        // Control toolbar
        en["toolbar.controls"]   = "Controls:";
        en["button.play"]        = "Play";
        en["button.resume"]      = "Resume";
        en["button.pause"]       = "Pause";
        en["button.stop"]        = "Stop";
        en["toolbar.running"]    = "Running: %s (%s)";

        // Footer
        en["footer.games"]       = "Games: %d";
        en["footer.emulator"]    = "Emulator: %s";
        en["footer.running"]     = "RUNNING";
        en["footer.idle"]        = "IDLE";
        en["footer.backend"]     = "Backend: %s";
        en["footer.active"]      = "active";
        en["footer.standby"]     = "standby";

        // Library panel
        en["library.header"]     = "GAME LIBRARY";
        en["library.titles"]     = "%d titles";
        en["library.search_hint"]= "Search games...";
        en["library.size_label"] = "Size:";

        // Console
        en["console.title"]      = "Console";

        // About dialog
        en["about.title"]        = "About pcsx5";
        en["about.line1"]        = "pcsx5 - PlayStation 5 HLE Emulator";
        en["about.line2"]        = "Backend: out-of-process pcsx5.exe";
        en["about.line3"]        = "UI: Dear ImGui + GLFW + OpenGL3";
        en["about.line4"]        = "Layout: top toolbar + grid + bottom console";
        en["about.ok"]           = "OK";

        // Messages / toasts
        en["msg.library_refreshed"]   = "Library refreshed (%d games)";
        en["msg.select_game"]         = "Select a game first.";
        en["msg.already_running"]     = "Already running, stop first.";
        en["msg.backend_not_found"]   = "Backend binary not found";
        en["msg.failed_spawn"]        = "Failed to spawn backend";
        en["msg.booting"]             = "Booting %s";
        en["msg.stopped"]             = "Stopped emulator";
        en["msg.resumed"]             = "Resumed emulator";
        en["msg.paused"]              = "Paused emulator";
        en["msg.touched_compat"]      = "Touched compat entry for %s";

        // System panel (src/ui/system_panel.cpp)
        en["system.header"]           = "SYSTEM";
        en["system.cpu"]              = "CPU";
        en["system.gpu"]              = "GPU";
        en["system.memory"]           = "Memory";
        en["system.os"]               = "OS";
        en["system.status"]           = "Status";
        en["system.info"]             = "System Information";
        en["system.cores"]            = "Cores";
        en["system.threads"]          = "Threads";
        en["system.ram"]              = "RAM";
        en["system.vram"]             = "VRAM";
        en["system.renderer"]         = "Renderer";
        en["system.api"]              = "API";
        en["system.version"]          = "Version";
        en["system.build"]            = "Build";
        en["system.host"]             = "Host";
        en["system.target"]           = "Target";
        en["system.snapshot"]         = "snapshot of the host hardware";
        en["system.refresh"]          = "Refresh";
        en["system.error_prefix"]     = "Error: %s";
        en["system.os_name"]          = "Name";
        en["system.os_kernel"]        = "Kernel";
        en["system.os_arch"]          = "Arch";
        en["system.cpu_brand"]        = "Brand";
        en["system.cpu_cores"]        = "Cores";
        en["system.cpu_base"]         = "Base";
        en["system.gpu_name"]         = "Name";
        en["system.gpu_vram"]         = "VRAM";
        en["system.gpu_shared"]       = "Shared";
        en["system.gpu_driver"]       = "Driver";
        en["system.memory_subtitle"]  = "Memory";
        en["system.unknown"]          = "Unknown";
        en["system.ram_label"]        = "RAM";
        en["system.memory_unavailable"] = "Memory info unavailable.";
        en["system.memory_note"]      = "Memory usage is sampled live; CPU / GPU / OS are queried on first\nload and on demand via the Refresh button.  Used to size the\nguest memory pool for booting PS5 binaries.";
        en["system.snapshot_failed"]  = "Snapshot() failed";
        en["system.cpu_ghz_cores"]    = "%.2f GHz, %dC/%dT";
        en["system.cpu_cores_only"]   = "%d cores";
        en["system.cpu_physical_logical"] = "%d physical / %d logical";
        en["system.cpu_base_ghz"]     = "%.2f GHz";

        // Help / usage
        en["help.usage"] = "Usage: pcsx5_ui [--games=DIR] [--compat=DIR] [--covers=DIR] [--backend=NAME]";
    }

    // -- All other languages: try to load from JSON, fall back to en-US pointers ---
    for (int i = 1; i < static_cast<int>(Language::Count); ++i) {
        Language lang = static_cast<Language>(i);
        Table& table = g_tables[i];
        
        if (!LoadJsonTable(lang, table)) {
            // Fallback: copy en-US pointers (shallow copy — both maps point to same const char* values)
            table = en;
        }
    }

    g_active_language = Language::EnUS;
    g_initialized = true;
}

// ---------------------------------------------------------------------------
// Cleanup function to free allocated strings
// ---------------------------------------------------------------------------
void Shutdown() {
    if (!g_initialized) return;
    
    for (int i = 0; i < static_cast<int>(Language::Count); ++i) {
        FreeTable(g_tables[i]);
    }
    g_initialized = false;
}

// ---------------------------------------------------------------------------
// Active language
// ---------------------------------------------------------------------------
void SetLanguage(Language lang) {
    if (!g_initialized) Init();
    if (lang >= Language::Count) lang = Language::EnUS;
    g_active_language = lang;
}

Language GetLanguage() {
    if (!g_initialized) Init();
    return g_active_language;
}

// ---------------------------------------------------------------------------
// Translate a key.  Fallback chain:
//   1. Current language table
//   2. en-US table
//   3. Return the key itself
// ---------------------------------------------------------------------------
const char* Tr(const char* key) {
    if (!g_initialized) Init();
    if (!key) return "";

    const Table& cur = g_tables[static_cast<int>(g_active_language)];
    auto it = cur.find(key);
    if (it != cur.end()) return it->second;

    // Fallback to en-US
    const Table& en = g_tables[static_cast<int>(Language::EnUS)];
    auto it2 = en.find(key);
    if (it2 != en.end()) return it2->second;

    // Unknown key — return the key itself so the UI shows something
    // recognisable instead of a blank.
    return key;
}

// ---------------------------------------------------------------------------
// Language metadata helpers
// ---------------------------------------------------------------------------
const char* LanguageName(Language lang) {
    switch (lang) {
        case Language::EnUS: return "English";
        case Language::JaJP: return "日本語";
        case Language::KoKR: return "한국어";
        case Language::ZhCN: return "简体中文";
        case Language::ZhTW: return "繁體中文";
        case Language::FrFR: return "Français";
        case Language::DeDE: return "Deutsch";
        case Language::EsES: return "Español";
        case Language::PtBR: return "Português";
        case Language::ItIT: return "Italiano";
        case Language::RuRU: return "Русский";
        default:             return "English";
    }
}

const char* LanguageTag(Language lang) {
    switch (lang) {
        case Language::EnUS: return "en-US";
        case Language::JaJP: return "ja-JP";
        case Language::KoKR: return "ko-KR";
        case Language::ZhCN: return "zh-CN";
        case Language::ZhTW: return "zh-TW";
        case Language::FrFR: return "fr-FR";
        case Language::DeDE: return "de-DE";
        case Language::EsES: return "es-ES";
        case Language::PtBR: return "pt-BR";
        case Language::ItIT: return "it-IT";
        case Language::RuRU: return "ru-RU";
        default:             return "en-US";
    }
}

Language LanguageFromString(const std::string& s) {
    // BCP-47 tags
    if (s == "en-US" || s == "en"  || s == "English") return Language::EnUS;
    if (s == "ja-JP" || s == "ja"  || s == "Japanese")return Language::JaJP;
    if (s == "ko-KR" || s == "ko"  || s == "Korean")  return Language::KoKR;
    if (s == "zh-CN" || s == "zh"  || s == "Chinese") return Language::ZhCN;
    if (s == "zh-TW" || s == "zh-TW")                 return Language::ZhTW;
    if (s == "fr-FR" || s == "fr"  || s == "French")  return Language::FrFR;
    if (s == "de-DE" || s == "de"  || s == "German")  return Language::DeDE;
    if (s == "es-ES" || s == "es"  || s == "Spanish") return Language::EsES;
    if (s == "pt-BR" || s == "pt"  || s == "Portuguese") return Language::PtBR;
    if (s == "it-IT" || s == "it"  || s == "Italian") return Language::ItIT;
    if (s == "ru-RU" || s == "ru"  || s == "Russian") return Language::RuRU;
    return Language::EnUS;
}

const std::vector<Language>& AllLanguages() {
    static const std::vector<Language> langs = {
        Language::EnUS, Language::JaJP, Language::KoKR,
        Language::ZhCN, Language::ZhTW, Language::FrFR,
        Language::DeDE, Language::EsES, Language::PtBR,
        Language::ItIT, Language::RuRU
    };
    return langs;
}

} // namespace I18n
} // namespace Ui