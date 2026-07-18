// Implementation of the compatibility database.  See compat.h for overview.

#include "compat.h"
#include "../common/log.h"

#include <algorithm>
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

// Portable strerror wrapper: returns std::string so the caller can concat.
static std::string LastErrorStr() {
#ifdef _WIN32
    char buf[256] = {};
    strerror_s(buf, sizeof(buf), errno);
    return std::string(buf);
#else
    return std::string(std::strerror(errno));
#endif
}

namespace Compat {

// ===========================================================================
// Status <-> string
// ===========================================================================
const char* StatusName(Status s) {
    switch (s) {
        case Status::Nothing:   return "nothing";
        case Status::Boot:      return "boot";
        case Status::Intro:     return "intro";
        case Status::Menus:     return "menus";
        case Status::Ingame:    return "ingame";
        case Status::Playable:  return "playable";
    }
    return "nothing";
}

bool StatusFromName(const std::string& s, Status& out) {
    if (s == "nothing")  { out = Status::Nothing;  return true; }
    if (s == "boot")     { out = Status::Boot;     return true; }
    if (s == "intro")    { out = Status::Intro;    return true; }
    if (s == "menus")    { out = Status::Menus;    return true; }
    if (s == "ingame")   { out = Status::Ingame;   return true; }
    if (s == "playable") { out = Status::Playable; return true; }
    // Legacy aliases (pre-unification scheme), kept for tolerance.
    if (s == "untested") { out = Status::Nothing;  return true; }
    if (s == "menu")     { out = Status::Menus;    return true; }
    if (s == "complete") { out = Status::Playable; return true; }
    return false;
}

// ===========================================================================
// Minimal JSON parser/writer (sufficient for our schema).
// ===========================================================================
namespace {

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
    bool is_null()   const { return type == Type::Null;   }
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
    std::size_t pos_ = 0;

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
        if (pos_ >= src_.size()) { if (err) *err = "unexpected end of input"; return false; }
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
        return false;
    }
    bool ParseNumber(JsonValue& v, std::string* err) {
        v.type = JsonValue::Type::Number;
        std::size_t start = pos_;
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
            for (std::size_t i = 0; i < v.a.size(); ++i) {
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

const JsonValue* Field(const JsonValue& obj, const char* key) {
    if (!obj.is_object()) return nullptr;
    auto it = obj.o.find(key);
    return it == obj.o.end() ? nullptr : &it->second;
}

JsonValue NullV()           { JsonValue v; v.type = JsonValue::Type::Null;   return v; }
JsonValue BoolV(bool b)     { JsonValue v; v.type = JsonValue::Type::Bool;   v.b = b; return v; }
JsonValue NumV(double n)    { JsonValue v; v.type = JsonValue::Type::Number; v.n = n; return v; }
JsonValue StrV(const std::string& s) { JsonValue v; v.type = JsonValue::Type::String; v.s = s; return v; }
JsonValue IntV(int i)       { return NumV(static_cast<double>(i)); }
JsonValue U64V(u64 i)       { return NumV(static_cast<double>(i)); }

JsonValue MakeObject()      { JsonValue v; v.type = JsonValue::Type::Object; return v; }
JsonValue MakeArray()       { JsonValue v; v.type = JsonValue::Type::Array;  return v; }

bool ReadFile(const std::string& path, std::string& out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "cannot open '" + path + "': " + LastErrorStr();
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool WriteFile(const std::string& path, const std::string& data, std::string* err) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (err) *err = "cannot open '" + path + "' for writing: " + LastErrorStr();
        return false;
    }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!f) {
        if (err) *err = "write failed: " + LastErrorStr();
        return false;
    }
    return true;
}

std::string ToLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ContainsCi(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    return ToLower(hay).find(ToLower(needle)) != std::string::npos;
}

std::string TimestampNow() {
    // Tiny ISO-8601 (UTC) helper without external deps.  Sufficient
    // resolution for human-readable compatibility entries.
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

}  // namespace

// ===========================================================================
// Service state + in-memory cache
// ===========================================================================
namespace {

std::string                  g_dir;
bool                         g_initialized = false;
std::map<std::string, Entry> g_cache;

void EnsureDir(const std::string& path, std::string* err) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        if (err) *err = "create_directories('" + path + "') failed: " + ec.message();
    }
}

std::string IndexPath() {
    return g_dir + "/compatibility.json";
}
std::string TitlesDir() {
    return g_dir + "/titles";
}
std::string TitlePath(const std::string& title_id) {
    return TitlesDir() + "/" + title_id + ".json";
}

}  // namespace

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------
void Initialize(const std::string& compat_dir) {
    g_dir = compat_dir;
    g_cache.clear();
    std::string err;
    EnsureDir(g_dir, &err);
    EnsureDir(TitlesDir(), &err);
    g_initialized = !g_dir.empty();
}

const std::string& Directory() { return g_dir; }
bool IsInitialized()           { return g_initialized; }

// ---------------------------------------------------------------------------
// Index file
// ---------------------------------------------------------------------------
namespace {

bool LoadIndex(std::vector<std::string>& titles, std::string* err) {
    titles.clear();
    const std::string path = IndexPath();
    std::string body;
    if (!ReadFile(path, body, err)) return false;
    JsonValue v;
    if (!JsonParser(body).Parse(v, err)) {
        if (err) *err = "compatibility.json: " + *err;
        return false;
    }
    if (const JsonValue* t = Field(v, "titles"); t && t->is_array()) {
        for (const auto& item : t->a) {
            if (item.is_string()) titles.push_back(item.s);
        }
    }
    std::sort(titles.begin(), titles.end());
    return true;
}

bool SaveIndex(const std::vector<std::string>& titles, std::string* err) {
    JsonValue root = MakeObject();
    root.o["schema_version"] = IntV(kCurrentSchemaVersion);
    root.o["description"]     = StrV("pcsx5 compatibility database index");
    JsonValue arr = MakeArray();
    for (const auto& t : titles) arr.a.push_back(StrV(t));
    root.o["titles"] = std::move(arr);
    std::ostringstream ss;
    WriteJson(ss, root, /*pretty=*/true, 0);
    return WriteFile(IndexPath(), ss.str(), err);
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry <-> JSON
// ---------------------------------------------------------------------------
namespace {

void EntryToJson(const Entry& e, JsonValue& out) {
    out = MakeObject();
    out.o["schema_version"] = IntV(kCurrentSchemaVersion);
    out.o["title_id"]       = StrV(e.title_id);
    out.o["name"]           = StrV(e.name);
    out.o["region"]         = StrV(e.region);
    out.o["version"]        = StrV(e.version);
    out.o["status"]         = StrV(StatusName(e.status));
    out.o["notes"]          = StrV(e.notes);
    out.o["curated_at"]     = StrV(e.curated_at);

    JsonValue wa = MakeArray();
    for (const auto& w : e.workarounds) wa.a.push_back(StrV(w));
    out.o["workarounds"] = std::move(wa);

    JsonValue au = MakeObject();
    au.o["last_tested"]                = StrV(e.auto_fields.last_tested);
    au.o["last_run_status"]            = StrV(e.auto_fields.last_run_status);
    au.o["last_run_git_revision"]      = StrV(e.auto_fields.last_run_git_revision);
    au.o["last_run_duration_ms"]       = NumV(e.auto_fields.last_run_duration_ms);
    au.o["last_run_resolved_imports"]  = U64V(e.auto_fields.last_run_resolved_imports);
    au.o["last_run_unresolved_imports"]= U64V(e.auto_fields.last_run_unresolved_imports);
    out.o["auto"] = std::move(au);
}

bool JsonToEntry(const JsonValue& v, Entry& e, std::string* err) {
    if (!v.is_object()) { if (err) *err = "root is not an object"; return false; }
    if (auto* p = Field(v, "title_id"); p && p->is_string()) e.title_id = p->s;
    if (auto* p = Field(v, "name");     p && p->is_string()) e.name     = p->s;
    if (auto* p = Field(v, "region");   p && p->is_string()) e.region   = p->s;
    if (auto* p = Field(v, "version");  p && p->is_string()) e.version  = p->s;
    if (auto* p = Field(v, "notes");    p && p->is_string()) e.notes    = p->s;
    if (auto* p = Field(v, "curated_at"); p && p->is_string()) e.curated_at = p->s;
    if (auto* p = Field(v, "status");   p && p->is_string()) {
        Status s;
        if (!StatusFromName(p->s, s)) {
            if (err) *err = "unknown status '" + p->s + "'";
            return false;
        }
        e.status = s;
    }
    e.workarounds.clear();
    if (auto* p = Field(v, "workarounds"); p && p->is_array()) {
        for (const auto& item : p->a) {
            if (item.is_string()) e.workarounds.push_back(item.s);
        }
    }
    if (auto* p = Field(v, "auto"); p && p->is_object()) {
        if (auto* q = Field(*p, "last_tested");       q && q->is_string()) e.auto_fields.last_tested = q->s;
        if (auto* q = Field(*p, "last_run_status");   q && q->is_string()) e.auto_fields.last_run_status = q->s;
        if (auto* q = Field(*p, "last_run_git_revision"); q && q->is_string()) e.auto_fields.last_run_git_revision = q->s;
        if (auto* q = Field(*p, "last_run_duration_ms"); q && q->is_number()) e.auto_fields.last_run_duration_ms = q->n;
        if (auto* q = Field(*p, "last_run_resolved_imports"); q && q->is_number())
            e.auto_fields.last_run_resolved_imports = static_cast<u64>(q->n);
        if (auto* q = Field(*p, "last_run_unresolved_imports"); q && q->is_number())
            e.auto_fields.last_run_unresolved_imports = static_cast<u64>(q->n);
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry CRUD
// ---------------------------------------------------------------------------
const Entry* Find(const std::string& title_id) {
    if (!g_initialized) return nullptr;
    auto it = g_cache.find(title_id);
    if (it != g_cache.end()) return &it->second;
    Entry e;
    if (!Load(title_id, e, nullptr)) return nullptr;
    auto [ins, _] = g_cache.emplace(title_id, std::move(e));
    return &ins->second;
}

bool Load(const std::string& title_id, Entry& out, std::string* err) {
    if (!g_initialized) { if (err) *err = "Compat not initialized"; return false; }
    const std::string path = TitlePath(title_id);
    std::string body;
    if (!ReadFile(path, body, err)) return false;
    JsonValue v;
    if (!JsonParser(body).Parse(v, err)) {
        if (err) *err = path + ": " + *err;
        return false;
    }
    if (!JsonToEntry(v, out, err)) return false;
    int from_version = 0;
    if (auto* p = Field(v, "schema_version"); p && p->is_number()) from_version = static_cast<int>(p->n);
    if (from_version != kCurrentSchemaVersion) {
        if (!MigrateToCurrent(from_version, out, err)) return false;
    }
    if (out.title_id.empty()) out.title_id = title_id;  // tolerate missing
    return true;
}

bool Save(Entry e, std::string* err) {
    if (!g_initialized) { if (err) *err = "Compat not initialized"; return false; }
    if (e.title_id.empty()) { if (err) *err = "title_id is required"; return false; }
    if (e.curated_at.empty()) e.curated_at = TimestampNow();

    std::vector<std::string> titles;
    LoadIndex(titles, nullptr);  // best effort; ignore errors
    if (std::find(titles.begin(), titles.end(), e.title_id) == titles.end()) {
        titles.push_back(e.title_id);
        std::sort(titles.begin(), titles.end());
        if (!SaveIndex(titles, err)) return false;
    }

    JsonValue v;
    EntryToJson(e, v);
    std::ostringstream ss;
    WriteJson(ss, v, /*pretty=*/true, 0);
    if (!WriteFile(TitlePath(e.title_id), ss.str(), err)) return false;

    g_cache[e.title_id] = std::move(e);
    return true;
}

bool Remove(const std::string& title_id, std::string* err) {
    if (!g_initialized) { if (err) *err = "Compat not initialized"; return false; }
    std::error_code ec;
    const std::string path = TitlePath(title_id);
    if (!std::filesystem::remove(path, ec) && ec) {
        if (err) *err = "remove failed: " + ec.message();
        return false;
    }
    std::vector<std::string> titles;
    LoadIndex(titles, nullptr);
    auto it = std::find(titles.begin(), titles.end(), title_id);
    if (it != titles.end()) {
        titles.erase(it);
        SaveIndex(titles, err);  // best effort
    }
    g_cache.erase(title_id);
    return true;
}

bool UpdateAuto(const std::string& title_id, const AutoFields& fields,
                std::string* err) {
    Entry e;
    if (!Load(title_id, e, err)) return false;
    e.auto_fields = fields;
    // Touch curated_at only if the entry didn't have one yet.
    if (e.curated_at.empty()) e.curated_at = TimestampNow();
    return Save(std::move(e), err);
}

// ---------------------------------------------------------------------------
// Listing / search
// ---------------------------------------------------------------------------
std::vector<std::string> ListTitles() {
    std::vector<std::string> titles;
    if (!g_initialized) return titles;
    LoadIndex(titles, nullptr);
    // Reconcile: append any title_id files on disk that aren't in the index
    // (defensive — e.g. someone dropped a file in manually).
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(TitlesDir(), ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (name.size() < 5 || name.substr(name.size() - 5) != ".json") continue;
        const std::string id = name.substr(0, name.size() - 5);
        if (std::find(titles.begin(), titles.end(), id) == titles.end()) {
            titles.push_back(id);
        }
    }
    std::sort(titles.begin(), titles.end());
    return titles;
}

std::vector<Entry> LoadAll() {
    std::vector<Entry> out;
    if (!g_initialized) return out;
    for (const auto& t : ListTitles()) {
        Entry e;
        if (Load(t, e, nullptr)) out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(),
              [](const Entry& a, const Entry& b) { return a.title_id < b.title_id; });
    return out;
}

std::vector<Entry> Search(const std::string& query) {
    std::vector<Entry> hits;
    for (auto& e : LoadAll()) {
        if (ContainsCi(e.title_id, query) ||
            ContainsCi(e.name,     query) ||
            ContainsCi(e.region,   query) ||
            ContainsCi(e.version,  query) ||
            ContainsCi(e.notes,    query) ||
            ContainsCi(StatusName(e.status), query)) {
            hits.push_back(std::move(e));
        }
    }
    return hits;
}

// ---------------------------------------------------------------------------
// Reports
// ------------------------------------------------------------------------==
std::string BuildMarkdownTable(const std::vector<Entry>& entries) {
    std::ostringstream ss;
    ss << "# pcsx5 Compatibility\n\n";
    ss << "| Title ID | Name | Region | Version | Status | Last Tested | Git | Notes |\n";
    ss << "|----------|------|--------|---------|--------|-------------|-----|-------|\n";
    for (const auto& e : entries) {
        ss << "| `" << e.title_id << "` "
           << "| " << (e.name.empty() ? "—" : e.name) << " "
           << "| " << (e.region.empty() ? "—" : e.region) << " "
           << "| " << (e.version.empty() ? "—" : e.version) << " "
           << "| " << StatusName(e.status) << " "
           << "| " << (e.auto_fields.last_tested.empty() ? "—" : e.auto_fields.last_tested) << " "
           << "| " << (e.auto_fields.last_run_git_revision.empty() ? "—" : e.auto_fields.last_run_git_revision.substr(0, 7)) << " "
           << "| " << (e.notes.empty() ? "—" : e.notes) << " |\n";
    }
    if (entries.empty()) {
        ss << "\n*(no entries yet — run `pcsx5_compat add <TITLE_ID> --name \"...\"`)*\n";
    }
    return ss.str();
}

bool WriteMarkdownReport(const std::string& path, std::string* err) {
    auto entries = LoadAll();
    const std::string body = BuildMarkdownTable(entries);
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    return WriteFile(path, body, err);
}

// ------------------------------------------------------------------------==
// Schema migration (no-op for v1)
// ==========================================================================
bool MigrateToCurrent(int from_version, Entry& e, std::string* /*err*/) {
    (void)e;
    if (from_version < 1) {
        // No migration defined for v0 -> v1; entries were not present.
    }
    return true;
}

}  // namespace Compat
