#pragma once
//
// Simple i18n / localisation system for pcsx5_ui.
//
// All user-visible strings are keyed by a short ASCII identifier and
// resolved through `I18n::Tr(key)`.  The active language is set once
// at startup (from CLI --lang= or config) and can be changed at runtime
// via the sidebar language combo.
//
// Adding a new language:
//   1. Add an entry to the `Language` enum.
//   2. Add a `case` in `LanguageName()` and `LanguageFromString()`.
//   3. Populate the string table in `InitLanguage()`.
//
// Currently only en-US is fully populated; other languages fall back
// to en-US for every key.
//

#include <string>
#include <unordered_map>
#include <vector>

namespace Ui {
namespace I18n {

// ---------------------------------------------------------------------------
// Supported languages
// ---------------------------------------------------------------------------
enum class Language {
    EnUS,   // English (United States) — default
    JaJP,   // Japanese
    KoKR,   // Korean
    ZhCN,   // Chinese (Simplified)
    ZhTW,   // Chinese (Traditional)
    FrFR,   // French
    DeDE,   // German
    EsES,   // Spanish
    PtBR,   // Portuguese (Brazil)
    ItIT,   // Italian
    RuRU,   // Russian

    Count
};

// ---------------------------------------------------------------------------
// Language helpers
// ---------------------------------------------------------------------------

// Human-readable name for a language (e.g. "English", "日本語").
const char* LanguageName(Language lang);

// Parse a BCP-47-style tag (e.g. "en-US", "ja-JP") into a Language enum.
// Returns EnUS for unrecognised strings.
Language LanguageFromString(const std::string& tag);

// Inverse of LanguageFromString: "en-US", "ja-JP", etc.
const char* LanguageTag(Language lang);

// Ordered list of all supported languages for UI combos.
const std::vector<Language>& AllLanguages();

// ---------------------------------------------------------------------------
// Translation API
// ---------------------------------------------------------------------------

// Set the active language.  Must be called before any Tr() call, or after
// the user changes language at runtime.
void SetLanguage(Language lang);

// Get the currently active language.
Language GetLanguage();

// Return the localised string for `key`.  Falls back to en-US if the key
// is missing in the current language, and returns `key` itself if the key
// is completely unknown (which should never happen in production).
const char* Tr(const char* key);

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

// Pre-populate all string tables.  Called once by RunUi().
void Init();

// Clean up allocated translation strings.  Called on application exit.
void Shutdown();

} // namespace I18n
} // namespace Ui