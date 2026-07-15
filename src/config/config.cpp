#include "config.h"
#include "../common/log.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

// ===========================================================================
// Defaults
// ===========================================================================
namespace ConfigService {
Config Config::Defaults() {
    return Config{}; // all fields initialised in the header
}
} // namespace ConfigService

// Bring ConfigService's section types into the outer anonymous namespace so
// the schema read/write helpers below can use them unqualified.
namespace {
using namespace ConfigService;

// ===========================================================================
// Tiny JSON parser/writer.  Sufficient for our schema; not a general-purpose
// library.  Stores everything as one of: object, array, string, number, bool.
// ===========================================================================
struct JsonValue;
using JsonObject = std::map<std::string, JsonValue>;
using JsonArray  = std::vector<JsonValue>;

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool        b = false;
    double      n = 0.0;
    std::string s;
    JsonArray   a;
    JsonObject  o;

    bool is_object() const { return type == Type::Object; }
    bool is_array()  const { return type == Type::Array;  }
    bool is_string() const { return type == Type::String; }
    bool is_number() const { return type == Type::Number; }
    bool is_bool()   const { return type == Type::Bool;   }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& src) : src_(src) {}

    bool Parse(JsonValue& out, std::string* err) {
        SkipWs();
        if (!ParseValue(out, err)) return false;
        SkipWs();
        if (pos_ != src_.size()) {
            if (err) *err = "trailing characters after JSON value";
            return false;
        }
        return true;
    }

private:
    const std::string& src_;
    size_t pos_ = 0;

    void SkipWs() {
        while (pos_ < src_.size() &&
               (src_[pos_] == ' ' || src_[pos_] == '\t' ||
                src_[pos_] == '\n' || src_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool Consume(char c) {
        SkipWs();
        if (pos_ < src_.size() && src_[pos_] == c) { ++pos_; return true; }
        return false;
    }

    bool ParseValue(JsonValue& v, std::string* err) {
        SkipWs();
        if (pos_ >= src_.size()) {
            if (err) *err = "unexpected end of input";
            return false;
        }
        char c = src_[pos_];
        if (c == '{') return ParseObject(v, err);
        if (c == '[') return ParseArray(v, err);
        if (c == '"') return ParseString(v, err);
        if (c == 't' || c == 'f') return ParseBool(v, err);
        if (c == 'n') return ParseNull(v, err);
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(v, err);
        if (err) *err = std::string("unexpected character '") + c + "'";
        return false;
    }

    bool ParseObject(JsonValue& v, std::string* err) {
        v.type = JsonValue::Type::Object;
        if (!Consume('{')) { if (err) *err = "expected '{'"; return false; }
        SkipWs();
        if (Consume('}')) return true;
        while (true) {
            SkipWs();
            JsonValue key;
            if (!ParseString(key, err)) return false;
            SkipWs();
            if (!Consume(':')) { if (err) *err = "expected ':'"; return false; }
            JsonValue val;
            if (!ParseValue(val, err)) return false;
            v.o[key.s] = std::move(val);
            SkipWs();
            if (Consume(',')) continue;
            if (Consume('}')) return true;
            if (err) *err = "expected ',' or '}'";
            return false;
        }
    }

    bool ParseArray(JsonValue& v, std::string* err) {
        v.type = JsonValue::Type::Array;
        if (!Consume('[')) { if (err) *err = "expected '['"; return false; }
        SkipWs();
        if (Consume(']')) return true;
        while (true) {
            JsonValue item;
            if (!ParseValue(item, err)) return false;
            v.a.push_back(std::move(item));
            SkipWs();
            if (Consume(',')) continue;
            if (Consume(']')) return true;
            if (err) *err = "expected ',' or ']'";
            return false;
        }
    }

    bool ParseString(JsonValue& v, std::string* err) {
        v.type = JsonValue::Type::String;
        if (!Consume('"')) { if (err) *err = "expected '\"'"; return false; }
        std::string out;
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') { v.s = std::move(out); return true; }
            if (c == '\\') {
                if (pos_ >= src_.size()) break;
                char e = src_[pos_++];
                switch (e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        if (pos_ + 4 > src_.size()) {
                            if (err) *err = "truncated \\u escape";
                            return false;
                        }
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = src_[pos_++];
                            cp <<= 4;
                            if      (h >= '0' && h <= '9') cp |= (h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                            else {
                                if (err) *err = "invalid hex in \\u escape";
                                return false;
                            }
                        }
                        // UTF-8 encode
                        if (cp < 0x80) {
                            out += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default:
                        if (err) *err = "unknown string escape";
                        return false;
                }
            } else {
                out += c;
            }
        }
        if (err) *err = "unterminated string";
        return false;
    }

    bool ParseBool(JsonValue& v, std::string* err) {
        v.type = JsonValue::Type::Bool;
        if (src_.compare(pos_, 4, "true") == 0)  { pos_ += 4; v.b = true;  return true; }
        if (src_.compare(pos_, 5, "false") == 0) { pos_ += 5; v.b = false; return true; }
        if (err) *err = "expected boolean";
        return false;
    }

    bool ParseNull(JsonValue& v, std::string* /*err*/) {
        v.type = JsonValue::Type::Null;
        if (src_.compare(pos_, 4, "null") == 0) { pos_ += 4; return true; }
        if (src_.compare(pos_, 4, "nul") == 0) { pos_ += 3; return true; } // tolerate "nul"
        return false;
    }

    bool ParseNumber(JsonValue& v, std::string* err) {
        v.type = JsonValue::Type::Number;
        size_t start = pos_;
        if (src_[pos_] == '-') ++pos_;
        while (pos_ < src_.size() &&
               ((src_[pos_] >= '0' && src_[pos_] <= '9') ||
                src_[pos_] == '.' || src_[pos_] == 'e' || src_[pos_] == 'E' ||
                src_[pos_] == '+' || src_[pos_] == '-')) {
            ++pos_;
        }
        std::string num = src_.substr(start, pos_ - start);
        if (num.empty() || num == "-") {
            if (err) *err = "invalid number";
            return false;
        }
        v.n = std::strtod(num.c_str(), nullptr);
        return true;
    }
};

// JSON serialiser (no indentation for compactness; pretty mode optional).
void WriteString(std::ostream& os, const std::string& s) {
    os << '"';
    for (char c : s) {
        switch (c) {
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\b': os << "\\b";  break;
            case '\f': os << "\\f";  break;
            case '\n': os << "\\n";  break;
            case '\r': os << "\\r";  break;
            case '\t': os << "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    os << buf;
                } else {
                    os << c;
                }
        }
    }
    os << '"';
}

void WriteJson(std::ostream& os, const JsonValue& v, bool pretty, int depth);

void WriteJson(std::ostream& os, const JsonValue& v, bool pretty, int depth) {
    switch (v.type) {
        case JsonValue::Type::Null:   os << "null"; break;
        case JsonValue::Type::Bool:   os << (v.b ? "true" : "false"); break;
        case JsonValue::Type::Number: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.17g", v.n);
            os << buf;
            break;
        }
        case JsonValue::Type::String: WriteString(os, v.s); break;
        case JsonValue::Type::Array: {
            os << '[';
            for (size_t i = 0; i < v.a.size(); ++i) {
                if (i) os << ',';
                if (pretty) os << "\n  " << std::string(depth * 2, ' ');
                WriteJson(os, v.a[i], pretty, depth + 1);
            }
            if (pretty && !v.a.empty()) os << "\n" << std::string(depth * 2, ' ');
            os << ']';
            break;
        }
        case JsonValue::Type::Object: {
            os << '{';
            bool first = true;
            for (const auto& [k, val] : v.o) {
                if (!first) os << ',';
                first = false;
                if (pretty) os << "\n" << std::string((depth + 1) * 2, ' ');
                WriteString(os, k);
                os << ':';
                if (pretty) os << ' ';
                WriteJson(os, val, pretty, depth + 1);
            }
            if (pretty && !v.o.empty()) os << "\n" << std::string(depth * 2, ' ');
            os << '}';
            break;
        }
    }
}

// ===========================================================================
// Schema <-> Config conversion
// ===========================================================================
LogLevel ParseLevel(const JsonValue& v, LogLevel fallback) {
    if (!v.is_string()) return fallback;
    const std::string& s = v.s;
    if (s == "Debug") return LogLevel::Debug;
    if (s == "Info")  return LogLevel::Info;
    if (s == "Warn")  return LogLevel::Warn;
    if (s == "Error") return LogLevel::Error;
    return fallback;
}

const char* LevelName(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "Debug";
        case LogLevel::Info:  return "Info";
        case LogLevel::Warn:  return "Warn";
        case LogLevel::Error: return "Error";
    }
    return "Info";
}

const JsonValue* Field(const JsonValue& obj, const char* key) {
    if (!obj.is_object()) return nullptr;
    auto it = obj.o.find(key);
    return it == obj.o.end() ? nullptr : &it->second;
}

JsonValue BoolV(bool b)   { JsonValue v; v.type = JsonValue::Type::Bool;   v.b = b;   return v; }
JsonValue NumV(double n)  { JsonValue v; v.type = JsonValue::Type::Number; v.n = n;   return v; }
JsonValue StrV(const std::string& s) { JsonValue v; v.type = JsonValue::Type::String; v.s = s; return v; }
JsonValue IntV(int i)     { return NumV(static_cast<double>(i)); }
JsonValue FltV(float f)   { return NumV(static_cast<double>(f)); }

void ReadLogging (const JsonValue& root, LoggingConfig&  l) {
    if (const JsonValue* v = Field(root, "logging"); v && v->is_object()) {
        if (auto* p = Field(*v, "min_level"))   l.min_level   = ParseLevel(*p, l.min_level);
        if (auto* p = Field(*v, "json_output")) l.json_output = (p->is_bool() ? p->b : l.json_output);
        if (auto* p = Field(*v, "file_path"))   l.file_path   = (p->is_string() ? p->s : l.file_path);
        if (auto* p = Field(*v, "file_append")) l.file_append = (p->is_bool() ? p->b : l.file_append);
    }
}

void ReadCrash   (const JsonValue& root, CrashConfig&    c) {
    if (const JsonValue* v = Field(root, "crash"); v && v->is_object()) {
        if (auto* p = Field(*v, "bundle_dir"))     c.bundle_dir     = (p->is_string() ? p->s : c.bundle_dir);
        if (auto* p = Field(*v, "write_minidump")) c.write_minidump = (p->is_bool()   ? p->b : c.write_minidump);
    }
}

void ReadHle     (const JsonValue& root, HleConfig&      h) {
    if (const JsonValue* v = Field(root, "hle"); v && v->is_object()) {
        if (auto* p = Field(*v, "strict_imports")) h.strict_imports = (p->is_bool() ? p->b : h.strict_imports);
        if (auto* p = Field(*v, "trace_calls"))    h.trace_calls    = (p->is_bool() ? p->b : h.trace_calls);
        if (auto* p = Field(*v, "trace_capacity")) h.trace_capacity = (p->is_number() ? static_cast<int>(p->n) : h.trace_capacity);
    }
}

void ReadGraphics(const JsonValue& root, GraphicsConfig& g) {
    if (const JsonValue* v = Field(root, "graphics"); v && v->is_object()) {
        if (auto* p = Field(*v, "width"))           g.width           = (p->is_number() ? static_cast<int>(p->n) : g.width);
        if (auto* p = Field(*v, "height"))          g.height          = (p->is_number() ? static_cast<int>(p->n) : g.height);
        if (auto* p = Field(*v, "fullscreen"))      g.fullscreen      = (p->is_bool()   ? p->b : g.fullscreen);
        if (auto* p = Field(*v, "renderer"))        g.renderer        = (p->is_number() ? static_cast<int>(p->n) : g.renderer);
        if (auto* p = Field(*v, "resolution_scale"))g.resolution_scale= (p->is_number() ? static_cast<float>(p->n) : g.resolution_scale);
    }
}

void ReadAudio   (const JsonValue& root, AudioConfig&    a) {
    if (const JsonValue* v = Field(root, "audio"); v && v->is_object()) {
        if (auto* p = Field(*v, "backend"))   a.backend   = (p->is_number() ? static_cast<int>(p->n) : a.backend);
        if (auto* p = Field(*v, "buffer_ms")) a.buffer_ms = (p->is_number() ? static_cast<int>(p->n) : a.buffer_ms);
        if (auto* p = Field(*v, "volume"))    a.volume    = (p->is_number() ? static_cast<float>(p->n) : a.volume);
    }
}

void ReadInput   (const JsonValue& root, InputConfig&    i) {
    if (const JsonValue* v = Field(root, "input"); v && v->is_object()) {
        if (auto* p = Field(*v, "backend")) i.backend  = (p->is_number() ? static_cast<int>(p->n) : i.backend);
        if (auto* p = Field(*v, "deadzone"))i.deadzone = (p->is_number() ? static_cast<float>(p->n) : i.deadzone);
        if (auto* p = Field(*v, "rumble"))  i.rumble   = (p->is_bool() ? p->b : i.rumble);
    }
}

void WriteLogging (JsonValue& root, const LoggingConfig&  l) {
    JsonValue v; v.type = JsonValue::Type::Object;
    v.o["min_level"]   = StrV(LevelName(l.min_level));
    v.o["json_output"] = BoolV(l.json_output);
    v.o["file_path"]   = StrV(l.file_path);
    v.o["file_append"] = BoolV(l.file_append);
    root.o["logging"]  = std::move(v);
}
void WriteCrash   (JsonValue& root, const CrashConfig&    c) {
    JsonValue v; v.type = JsonValue::Type::Object;
    v.o["bundle_dir"]     = StrV(c.bundle_dir);
    v.o["write_minidump"] = BoolV(c.write_minidump);
    root.o["crash"]       = std::move(v);
}
void WriteHle     (JsonValue& root, const HleConfig&      h) {
    JsonValue v; v.type = JsonValue::Type::Object;
    v.o["strict_imports"] = BoolV(h.strict_imports);
    v.o["trace_calls"]    = BoolV(h.trace_calls);
    v.o["trace_capacity"] = IntV(h.trace_capacity);
    root.o["hle"]         = std::move(v);
}
void WriteGraphics(JsonValue& root, const GraphicsConfig& g) {
    JsonValue v; v.type = JsonValue::Type::Object;
    v.o["width"]            = IntV(g.width);
    v.o["height"]           = IntV(g.height);
    v.o["fullscreen"]       = BoolV(g.fullscreen);
    v.o["renderer"]         = IntV(g.renderer);
    v.o["resolution_scale"] = FltV(g.resolution_scale);
    root.o["graphics"]      = std::move(v);
}
void WriteAudio   (JsonValue& root, const AudioConfig&    a) {
    JsonValue v; v.type = JsonValue::Type::Object;
    v.o["backend"]   = IntV(a.backend);
    v.o["buffer_ms"] = IntV(a.buffer_ms);
    v.o["volume"]    = FltV(a.volume);
    root.o["audio"]  = std::move(v);
}
void WriteInput   (JsonValue& root, const InputConfig&    i) {
    JsonValue v; v.type = JsonValue::Type::Object;
    v.o["backend"]  = IntV(i.backend);
    v.o["deadzone"] = FltV(i.deadzone);
    v.o["rumble"]   = BoolV(i.rumble);
    root.o["input"]  = std::move(v);
}

void ReadUi      (const JsonValue& root, UiConfig&       u) {
    if (const JsonValue* v = Field(root, "ui"); v && v->is_object()) {
        if (auto* p = Field(*v, "language")) u.language = (p->is_string() ? p->s : u.language);
    }
}

void WriteUi     (JsonValue& root, const UiConfig&       u) {
    JsonValue v; v.type = JsonValue::Type::Object;
    v.o["language"] = StrV(u.language);
    root.o["ui"]    = std::move(v);
}

} // namespace (json primitives + parser)

// ===========================================================================
// Schema <-> Config conversion (ConfigService-internal)
// ===========================================================================
namespace ConfigService {
namespace {
std::string g_dir;
bool        g_initialized = false;
Config      g_global;
std::map<std::string, Config> g_per_title; // cached per-title overrides
} // namespace

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------
void Initialize(const std::string& config_dir) {
    g_dir = config_dir;
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);
    std::filesystem::create_directories(g_dir + "/titles", ec);

    Config cfg = Config::Defaults();
    std::string err;
    const std::string path = g_dir + "/global.json";
    if (std::filesystem::exists(path)) {
        if (LoadFromFile(path, cfg, &err)) {
            LOG_INFO(General, "Loaded global config (schema v%d) from %s",
                     cfg.schema_version, path.c_str());
        } else {
            LOG_WARN(General, "Failed to parse global config (%s); using defaults.", err.c_str());
            cfg = Config::Defaults();
        }
    } else {
        LOG_INFO(General, "No global config at %s; writing defaults.", path.c_str());
        cfg = Config::Defaults();
        cfg.loaded_from_disk = true; // we just persisted it ourselves
        SaveToFile(path, cfg, &err);
    }
    g_global = std::move(cfg);

    // Refresh per-title cache by scanning titles/*.json.  Files that fail to
    // parse are skipped with a warning; missing files simply mean "no
    // per-title overrides yet" and do not produce a log line.
    g_per_title.clear();
    std::error_code lec;
    for (const auto& entry : std::filesystem::directory_iterator(g_dir + "/titles", lec)) {
        if (!entry.is_regular_file()) continue;
        const std::string fname = entry.path().filename().string();
        if (fname.size() < 5 || fname.substr(fname.size() - 5) != ".json") continue;
        const std::string title_id = fname.substr(0, fname.size() - 5);
        Config tcfg;
        std::string terr;
        if (LoadFromFile(entry.path().string(), tcfg, &terr)) {
            g_per_title.emplace(title_id, std::move(tcfg));
        } else {
            LOG_WARN(General, "Skipping per-title config %s: %s",
                     fname.c_str(), terr.c_str());
        }
    }

    g_initialized = true;
}

bool IsInitialized() { return g_initialized; }
const std::string& Directory() { return g_dir; }

Config& MutableGlobal() {
    if (!g_initialized) Initialize("./pcsx5_config");
    return g_global;
}
const Config& Global() {
    if (!g_initialized) Initialize("./pcsx5_config");
    return g_global;
}

Config EffectiveFor(const std::string& title_id) {
    Config eff = g_global;
    auto it = g_per_title.find(title_id);
    if (it != g_per_title.end()) {
        const Config& t = it->second;
        // Per-title overrides: any field marked as "user-touched" wins.
        // Simpler policy: per-title fully replaces the section when the
        // per-title file declares the section.  Since we always write the
        // full schema on save, we just overlay all sections.
        eff.logging  = t.logging;
        eff.crash    = t.crash;
        eff.hle      = t.hle;
        eff.graphics = t.graphics;
        eff.audio    = t.audio;
        eff.input    = t.input;
        eff.schema_version = t.schema_version;
        eff.loaded_from_disk = t.loaded_from_disk;
    }
    return eff;
}

const Config* ForTitle(const std::string& title_id) {
    auto it = g_per_title.find(title_id);
    return it == g_per_title.end() ? nullptr : &it->second;
}

Config& MutableForTitle(const std::string& title_id) {
    if (!g_initialized) Initialize("./pcsx5_config");
    auto it = g_per_title.find(title_id);
    if (it == g_per_title.end()) {
        g_per_title[title_id] = g_global; // start from global, mutate later
        it = g_per_title.find(title_id);
    }
    return it->second;
}

bool SaveGlobal() {
    const std::string path = g_dir + "/global.json";
    std::string err;
    if (!SaveToFile(path, g_global, &err)) {
        LOG_ERROR(General, "Failed to write global config: %s", err.c_str());
        return false;
    }
    return true;
}

bool SavePerTitle(const std::string& title_id) {
    if (title_id.empty()) return false;
    std::filesystem::create_directories(g_dir + "/titles");
    const std::string path = g_dir + "/titles/" + title_id + ".json";
    auto it = g_per_title.find(title_id);
    if (it == g_per_title.end()) return false;
    std::string err;
    if (!SaveToFile(path, it->second, &err)) {
        LOG_ERROR(General, "Failed to write per-title config: %s", err.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------
bool LoadFromFile(const std::string& path, Config& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "could not open file for reading";
        return false;
    }
    std::ostringstream os;
    os << in.rdbuf();
    std::string text = os.str();

    JsonParser p(text);
    JsonValue root;
    std::string perr;
    if (!p.Parse(root, &perr)) {
        if (error) *error = "parse error: " + perr;
        return false;
    }
    if (!root.is_object()) {
        if (error) *error = "top-level value is not an object";
        return false;
    }

    int version = kCurrentSchemaVersion;
    {
        const JsonValue* pv = Field(root, "schema_version");
        if (pv && pv->is_number()) {
            version = static_cast<int>(pv->n);
        } else if (pv && pv->is_string()) {
            // tolerate "1" as a string
            try { version = std::stoi(pv->s); } catch (...) {}
        }
    }

    out = Config::Defaults();
    ReadLogging (root, out.logging);
    ReadCrash   (root, out.crash);
    ReadHle     (root, out.hle);
    ReadGraphics(root, out.graphics);
    ReadAudio   (root, out.audio);
    ReadInput   (root, out.input);
    ReadUi      (root, out.ui);

    out.schema_version   = version;
    out.loaded_from_disk = true;

    if (version != kCurrentSchemaVersion) {
        std::string merr;
        if (!MigrateToCurrent(version, out, &merr)) {
            if (error) *error = "migration failed: " + merr;
            return false;
        }
    }
    return true;
}

bool SaveToFile(const std::string& path, const Config& cfg, std::string* error) {
    JsonValue root;
    root.type = JsonValue::Type::Object;
    root.o["schema_version"] = IntV(kCurrentSchemaVersion);
    WriteLogging (root, cfg.logging);
    WriteCrash   (root, cfg.crash);
    WriteHle     (root, cfg.hle);
    WriteGraphics(root, cfg.graphics);
    WriteAudio   (root, cfg.audio);
    WriteInput   (root, cfg.input);
    WriteUi      (root, cfg.ui);

    std::ofstream out(path, std::ios::trunc | std::ios::binary);
    if (!out) {
        if (error) *error = "could not open file for writing";
        return false;
    }
    WriteJson(out, root, /*pretty=*/true, /*depth=*/0);
    out << "\n";
    return out.good();
}

bool MigrateToCurrent(int from_version, Config& cfg, std::string* error) {
    if (from_version == kCurrentSchemaVersion) return true;
    if (from_version > kCurrentSchemaVersion) {
        LOG_WARN(General,
                 "Config schema v%d is newer than supported v%d; passing through unchanged.",
                 from_version, kCurrentSchemaVersion);
        cfg.schema_version = kCurrentSchemaVersion;
        return true;
    }
    if (from_version < 1) {
        if (error) *error = "unknown pre-v1 schema";
        return false;
    }
    // v1 -> v2: add UiConfig with language field
    if (from_version == 1) {
        cfg.ui.language = "en-US";
    }
    cfg.schema_version = kCurrentSchemaVersion;
    return true;
}

} // namespace ConfigService
