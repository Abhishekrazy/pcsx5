#pragma once
//
// Versioned configuration service.
//
// Two layers:
//   * `Global`    — global.json  in <config_dir>/global.json
//   * `PerTitle`  — titles/<title_id>.json  for per-title overrides
//
// On disk every file is a JSON object with a leading `schema_version` field.
// Older schemas are upgraded through `MigrateToCurrent` before being applied;
// a corrupt or missing file falls back to the compiled-in defaults in
// `Config::Defaults()`.
//
// Lookup is layered: callers ask for `EffectiveConfig(title_id)` which returns
// the global settings with any per-title overrides applied.  Callers can also
// mutate the per-title view directly via `MutableForTitle(title_id)` and
// persist it with `SavePerTitle(title_id)`.
//
#include "../common/log.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ConfigService {

// ---------------------------------------------------------------------------
// Schema versioning
// ---------------------------------------------------------------------------
inline constexpr int kCurrentSchemaVersion = 3;

// ---------------------------------------------------------------------------
// Sectioned config.  Each section is plain data so it can be serialised /
// migrated independently.
// ---------------------------------------------------------------------------
struct LoggingConfig {
    LogLevel  min_level   = LogLevel::Info;   // minimum per-category level
    bool      json_output = false;            // emit JSON objects to stdout
    std::string file_path;                    // empty = no file mirror
    bool      file_append = false;            // append vs truncate
};

struct CrashConfig {
    std::string bundle_dir    = "pcsx5_crash"; // root for crash bundles
    bool        write_minidump = true;         // include minidump.dmp
};

struct HleConfig {
    bool    strict_imports = false;            // fail on unresolved imports
    bool    trace_calls    = true;             // record into the trace ring
    int     trace_capacity = 256;              // trace ring size
};

struct GraphicsConfig {
    int   width            = 1280;
    int   height           = 720;
    bool  fullscreen       = false;
    int   renderer         = 0;                // 0=Vulkan, 1=OpenGL (future)
    float resolution_scale = 1.0f;             // 0.5..2.0
};

struct AudioConfig {
    int   backend   = 0;                       // 0=Off, 1=WASAPI, 2=XAudio2
    int   buffer_ms = 50;
    float volume    = 1.0f;
};

struct InputConfig {
    int   backend = 0;                         // 0=SDL, 1=XInput
    float deadzone = 0.15f;
    bool  rumble   = true;
};

struct UiConfig {
    std::string language = "en-US";            // BCP-47 tag, e.g. "en-US", "ja-JP"
};

struct LoaderConfig {
    // Directory holding user-supplied firmware PRX/SPRX modules.  Empty
    // disables firmware-module resolution; unresolved modules fall back to
    // the HLE implementations.  Game-bundled modules (<gamedir>/sce_module/)
    // are always searched first regardless of this setting.
    std::string firmware_modules_dir;
};

// ---------------------------------------------------------------------------
// Multi-user profile model (global-only; per-title overrides never touch it).
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t kFirstUserId = 0x10000000; // first local user id

struct UserProfile {
    std::uint32_t id = kFirstUserId;     // stable local user id
    std::string   name      = "Player";  // display name
    std::string   online_id = "Player";  // PSN-style online id
};

struct UsersConfig {
    // Default install: a single local "Player" profile.
    std::vector<UserProfile> profiles = {UserProfile{}};
    int                      active_user = 0; // index into `profiles`
};

// Top-level config — covers both global defaults and per-title overrides.
// When loaded as a per-title file, only the fields the user touched are
// populated; the rest should fall back to the global view at lookup time.
struct Config {
    LoggingConfig  logging;
    CrashConfig    crash;
    HleConfig      hle;
    GraphicsConfig graphics;
    AudioConfig    audio;
    InputConfig    input;
    UiConfig       ui;
    LoaderConfig   loader;
    UsersConfig    users;   // global-only; EffectiveFor never overlays it

    // -- helpers -----------------------------------------------------------
    static Config Defaults();                   // compiled-in defaults

    // True if the caller populated this config from disk (i.e. it is not a
    // pure defaults object).  Used by tests + the migration warning path.
    bool loaded_from_disk = false;
    int  schema_version   = 0;                 // 0 = never persisted
};

// ---------------------------------------------------------------------------
// Service API
// ---------------------------------------------------------------------------

// Bind the service to a directory (created if missing).  Calling Initialize
// twice with different directories is supported but discouraged.  Loads the
// global config immediately.  Idempotent.
void Initialize(const std::string& config_dir);

// True if Initialize has been called.  All other functions assert on this
// (except SetForTitle which queues the value).
bool IsInitialized();

// Returns the absolute config directory currently in use.
const std::string& Directory();

// Return the live global config.  Mutate fields directly; the changes are
// NOT auto-persisted.  Call `SaveGlobal()` to write back.
Config& MutableGlobal();
const Config& Global();

// Apply per-title overrides to a fresh copy of the global config and return
// the effective view.  Empty `title_id` returns the global config.
Config EffectiveFor(const std::string& title_id);

// Get / set / save per-title overrides.  A title's config file is created on
// first write.  `MutableForTitle` lazily creates a per-title record backed
// by an empty (defaults) file until SavePerTitle is called.
Config& MutableForTitle(const std::string& title_id);
const Config* ForTitle(const std::string& title_id);   // nullptr if unset

// Persist the in-memory config to disk.  Always rewrites the schema_version
// field to `kCurrentSchemaVersion`.
bool SaveGlobal();
bool SavePerTitle(const std::string& title_id);

// I/O primitives — used by tests, also by the public save functions.
bool LoadFromFile(const std::string& path, Config& out, std::string* error);
bool SaveToFile(const std::string& path, const Config& cfg, std::string* error);

// ---------------------------------------------------------------------------
// User profile accessors (global view only — per-title files never carry
// users).  These are the accessors the HLE user-service / libpad user-id
// validation should consume instead of hardcoding 0x10000000 / "Player1".
// ---------------------------------------------------------------------------

// Active profile, or nullptr when the profile list is empty / index invalid.
const UserProfile* ActiveUserProfile();

// Look up a profile by its stable user id; nullptr when unknown.
const UserProfile* FindUserProfile(std::uint32_t id);

// Convenience: id of the active profile, or 0 when none exists.
std::uint32_t ActiveUserId();

// Migrate a config read from an older schema version to the current schema.
// Returns true on success.  `from_version` is the version stamp on disk;
// `cfg` is the deserialised result.  Unknown future versions log a warning
// and pass through unchanged.
bool MigrateToCurrent(int from_version, Config& cfg, std::string* error);

} // namespace ConfigService
