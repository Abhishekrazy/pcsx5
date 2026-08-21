#include "libjson.h"
#include "hle.h"
#include "../memory/memory.h"
#include "../common/log.h"

#include <cstring>
#include <cstdio>
#include <map>
#include <vector>
#include <string>
#include <mutex>

// sce::Json (libSceJson) HLE — ported from KytyPS5's libJson2.cpp.
//
// Guest ABI (Orbis sce::Json):
//   JsonValue  = 32 bytes: [0]parent (8) [8]rootparam (8) [16]value union (8)
//                          [24]padding (4) [28]type (4)
//   JsonString / JsonArray / JsonObject = 8 bytes: pointer to a host-side
//   std::string / std::vector<JsonValue*> / std::map<std::string, JsonValue*>.
//   type codes: 0=null 1=boolean 2=integer 3=unsigned 4=real 5=string 6=array 7=object
//
// Guest addresses are host addresses here (direct-mapped guest memory), so the
// guest's JsonValue/JsonString/... pointers can be treated as host pointers and
// the impl objects live on the host heap.

namespace HLE {
namespace {

enum JsonValueType : u32 {
    JsonValueTypeNull     = 0,
    JsonValueTypeBoolean  = 1,
    JsonValueTypeInteger  = 2,
    JsonValueTypeUnsigned = 3,
    JsonValueTypeReal     = 4,
    JsonValueTypeString   = 5,
    JsonValueTypeArray    = 6,
    JsonValueTypeObject   = 7,
};

constexpr int32_t JSON_ERROR_PARSE_INVALID_CHAR = -2138799871; // 0x80848101
constexpr int32_t JSON_ERROR_INVALID_ARGUMENT   = -2138799840; // 0x80848120

struct JsonValue;

struct JsonString {
    std::string* impl;
};

struct JsonArray {
    std::vector<JsonValue*>* impl;
};

struct JsonObject {
    std::map<std::string, JsonValue*>* impl;
};

struct JsonValue {
    JsonValue* parent;
    void*      rootparam;
    union {
        bool        boolean;
        int64_t     integer;
        uint64_t    uinteger;
        double      real;
        JsonString* string;
        JsonArray*  array;
        JsonObject* object;
    };
    char     padding[4];
    uint32_t type;
};

struct JsonInitParameter2 {
    void*    allocator;
    void*    user_data;
    size_t   file_buffer_size;
    uint32_t special_float_format_type;
    uint32_t reserved[3];
};

// ---------------------------------------------------------------------------
// Host-side impl accessors (lazily allocate the std container)
// ---------------------------------------------------------------------------
static std::string* JsonStringImpl(JsonString* self) {
    if (self == nullptr) return nullptr;
    if (self->impl == nullptr) self->impl = new std::string;
    return self->impl;
}

static std::vector<JsonValue*>* JsonArrayImpl(JsonArray* self) {
    if (self == nullptr) return nullptr;
    if (self->impl == nullptr) self->impl = new std::vector<JsonValue*>;
    return self->impl;
}

static std::map<std::string, JsonValue*>* JsonObjectImpl(JsonObject* self) {
    if (self == nullptr) return nullptr;
    if (self->impl == nullptr) self->impl = new std::map<std::string, JsonValue*>;
    return self->impl;
}

static JsonValue* JsonValueNew();
static void JsonValueClear(JsonValue* self);
static void JsonValueDelete(JsonValue* self);

static JsonString* JsonStringNew(const std::string& value) {
    auto* str = new JsonString {};
    str->impl = new std::string(value);
    return str;
}

static JsonArray* JsonArrayNew() {
    auto* array = new JsonArray {};
    array->impl = new std::vector<JsonValue*>;
    return array;
}

static JsonObject* JsonObjectNew() {
    auto* object = new JsonObject {};
    object->impl = new std::map<std::string, JsonValue*>;
    return object;
}

static void JsonValueInit(JsonValue* self) {
    if (self != nullptr) {
        std::memset(self, 0, sizeof(JsonValue));
        self->type = JsonValueTypeNull;
    }
}

static void JsonStringDelete(JsonString* self) {
    if (self != nullptr) {
        delete self->impl;
        delete self;
    }
}

static void JsonArrayDelete(JsonArray* self) {
    if (self != nullptr) {
        if (self->impl != nullptr) {
            for (auto* value : *self->impl) {
                JsonValueDelete(value);
            }
        }
        delete self->impl;
        delete self;
    }
}

static void JsonObjectDelete(JsonObject* self) {
    if (self != nullptr) {
        if (self->impl != nullptr) {
            for (auto& item : *self->impl) {
                JsonValueDelete(item.second);
            }
            self->impl->clear();
        }
        delete self->impl;
        self->impl = nullptr;
    }
}

static void JsonValueClear(JsonValue* self) {
    if (self == nullptr) return;
    switch (self->type) {
        case JsonValueTypeString:
            if (self->string != nullptr) {
                JsonStringDelete(self->string);
                self->string = nullptr;
            }
            break;
        case JsonValueTypeArray:
            if (self->array != nullptr) {
                JsonArrayDelete(self->array);
                self->array = nullptr;
            }
            break;
        case JsonValueTypeObject:
            if (self->object != nullptr) {
                JsonObjectDelete(self->object);
                self->object = nullptr;
            }
            break;
        default:
            break;
    }
    std::memset(self, 0, sizeof(JsonValue));
    self->type = JsonValueTypeNull;
}

static JsonValue* JsonValueNew() {
    auto* value = new JsonValue;
    JsonValueInit(value);
    return value;
}

static void JsonValueDelete(JsonValue* self) {
    if (self != nullptr) {
        JsonValueClear(self);
        delete self;
    }
}

static void JsonValueCopy(JsonValue* dst, const JsonValue* src) {
    if (dst == nullptr || src == nullptr) return;
    JsonValueClear(dst);
    JsonValueInit(dst);
    switch (src->type) {
        case JsonValueTypeBoolean:
            dst->type    = src->type;
            dst->boolean = src->boolean;
            break;
        case JsonValueTypeInteger:
            dst->type    = src->type;
            dst->integer = src->integer;
            break;
        case JsonValueTypeUnsigned:
            dst->type     = src->type;
            dst->uinteger = src->uinteger;
            break;
        case JsonValueTypeReal:
            dst->type = src->type;
            dst->real = src->real;
            break;
        case JsonValueTypeString:
            dst->type   = src->type;
            dst->string = JsonStringNew(*JsonStringImpl(src->string));
            break;
        case JsonValueTypeArray: {
            dst->type  = src->type;
            dst->array = JsonArrayNew();
            if (src->array != nullptr && src->array->impl != nullptr) {
                for (auto* v : *src->array->impl) {
                    JsonValue* nv = JsonValueNew();
                    JsonValueCopy(nv, v);
                    dst->array->impl->push_back(nv);
                }
            }
            break;
        }
        case JsonValueTypeObject: {
            dst->type   = src->type;
            dst->object = JsonObjectNew();
            if (src->object != nullptr && src->object->impl != nullptr) {
                for (auto& item : *src->object->impl) {
                    JsonValue* nv = JsonValueNew();
                    JsonValueCopy(nv, item.second);
                    (*dst->object->impl)[item.first] = nv;
                }
            }
            break;
        }
        default:
            break;
    }
    dst->parent = src->parent;
}

static JsonValue* JsonObjectLookup(JsonObject* object, const std::string& key, bool create) {
    auto* impl = JsonObjectImpl(object);
    if (impl == nullptr) return nullptr;
    auto it = impl->find(key);
    if (it != impl->end()) return it->second;
    if (!create) return nullptr;
    JsonValue* v = JsonValueNew();
    (*impl)[key] = v;
    return v;
}

// ---------------------------------------------------------------------------
// JSON parser (minimal recursive-descent, enough for data/config files)
// ---------------------------------------------------------------------------
struct JsonParserState {
    const char* p;
    const char* end;
};

static void SkipWs(JsonParserState& st) {
    while (st.p < st.end && (*st.p == ' ' || *st.p == '\t' || *st.p == '\n' || *st.p == '\r')) ++st.p;
}

static bool ParseString(JsonParserState& st, std::string& out) {
    SkipWs(st);
    if (st.p >= st.end || *st.p != '"') return false;
    ++st.p;
    out.clear();
    while (st.p < st.end) {
        const char c = *st.p;
        if (c == '"') { ++st.p; return true; }
        if (c == '\\' && st.p + 1 < st.end) {
            const char e = st.p[1];
            st.p += 2;
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                default:  out += e;    break;
            }
            continue;
        }
        out += c;
        ++st.p;
    }
    return false;
}

static bool ParseNumber(JsonParserState& st, JsonValue* v) {
    SkipWs(st);
    const char* start = st.p;
    bool is_real = false;
    if (st.p < st.end && (*st.p == '-' || *st.p == '+')) ++st.p;
    while (st.p < st.end && ((*st.p >= '0' && *st.p <= '9') || *st.p == '.' || *st.p == 'e' || *st.p == 'E' || *st.p == '-' || *st.p == '+')) {
        if (*st.p == '.' || *st.p == 'e' || *st.p == 'E') is_real = true;
        ++st.p;
    }
    if (st.p == start) return false;
    std::string tok(start, static_cast<size_t>(st.p - start));
    if (is_real) {
        v->type = JsonValueTypeReal;
        v->real = std::strtod(tok.c_str(), nullptr);
    } else {
        // Signed vs unsigned: a leading '-' makes it integer.
        if (start[0] == '-') {
            v->type    = JsonValueTypeInteger;
            v->integer = std::strtoll(tok.c_str(), nullptr, 10);
        } else {
            v->type     = JsonValueTypeUnsigned;
            v->uinteger = std::strtoull(tok.c_str(), nullptr, 10);
        }
    }
    return true;
}

static bool ParseValue(JsonParserState& st, JsonValue* v);

static bool ParseArray(JsonParserState& st, JsonValue* v) {
    SkipWs(st);
    if (st.p >= st.end || *st.p != '[') return false;
    ++st.p;
    v->type  = JsonValueTypeArray;
    v->array = JsonArrayNew();
    SkipWs(st);
    if (st.p < st.end && *st.p == ']') { ++st.p; return true; }
    for (;;) {
        JsonValue* elem = JsonValueNew();
        if (!ParseValue(st, elem)) { JsonValueDelete(elem); return false; }
        v->array->impl->push_back(elem);
        SkipWs(st);
        if (st.p >= st.end) return false;
        if (*st.p == ',') { ++st.p; continue; }
        if (*st.p == ']') { ++st.p; return true; }
        return false;
    }
}

static bool ParseObject(JsonParserState& st, JsonValue* v) {
    SkipWs(st);
    if (st.p >= st.end || *st.p != '{') return false;
    ++st.p;
    v->type   = JsonValueTypeObject;
    v->object = JsonObjectNew();
    SkipWs(st);
    if (st.p < st.end && *st.p == '}') { ++st.p; return true; }
    for (;;) {
        std::string key;
        if (!ParseString(st, key)) return false;
        SkipWs(st);
        if (st.p >= st.end || *st.p != ':') return false;
        ++st.p;
        JsonValue* val = JsonValueNew();
        if (!ParseValue(st, val)) { JsonValueDelete(val); return false; }
        (*v->object->impl)[key] = val;
        SkipWs(st);
        if (st.p >= st.end) return false;
        if (*st.p == ',') { ++st.p; continue; }
        if (*st.p == '}') { ++st.p; return true; }
        return false;
    }
}

static bool ParseValue(JsonParserState& st, JsonValue* v) {
    SkipWs(st);
    if (st.p >= st.end) return false;
    if (*st.p == '{') return ParseObject(st, v);
    if (*st.p == '[') return ParseArray(st, v);
    if (*st.p == '"') {
        std::string s;
        if (!ParseString(st, s)) return false;
        v->type   = JsonValueTypeString;
        v->string = JsonStringNew(s);
        return true;
    }
    if (st.p + 4 <= st.end && std::memcmp(st.p, "true", 4) == 0) {
        st.p += 4;
        v->type = JsonValueTypeBoolean;
        v->boolean = true;
        return true;
    }
    if (st.p + 5 <= st.end && std::memcmp(st.p, "false", 5) == 0) {
        st.p += 5;
        v->type = JsonValueTypeBoolean;
        v->boolean = false;
        return true;
    }
    if (st.p + 4 <= st.end && std::memcmp(st.p, "null", 4) == 0) {
        st.p += 4;
        v->type = JsonValueTypeNull;
        return true;
    }
    return ParseNumber(st, v);
}

// ---------------------------------------------------------------------------
// Serialization (for JsonValueSerialize)
// ---------------------------------------------------------------------------
static void SerializeValue(const JsonValue* value, std::string* out) {
    if (value == nullptr) { out->append("null"); return; }
    switch (value->type) {
        case JsonValueTypeNull:    out->append("null"); break;
        case JsonValueTypeBoolean: out->append(value->boolean ? "true" : "false"); break;
        case JsonValueTypeInteger: out->append(std::to_string(value->integer)); break;
        case JsonValueTypeUnsigned: out->append(std::to_string(value->uinteger)); break;
        case JsonValueTypeReal: {
            char buf[32] = {};
            snprintf(buf, sizeof(buf), "%g", value->real);
            out->append(buf);
            break;
        }
        case JsonValueTypeString: {
            out->push_back('"');
            const std::string& s = *JsonStringImpl(value->string);
            for (char c : s) {
                if (c == '"' || c == '\\') out->push_back('\\');
                out->push_back(c);
            }
            out->push_back('"');
            break;
        }
        case JsonValueTypeArray: {
            out->push_back('[');
            const std::vector<JsonValue*>* arr = JsonArrayImpl(value->array);
            bool first = true;
            for (auto* v : *arr) {
                if (!first) out->push_back(',');
                first = false;
                SerializeValue(v, out);
            }
            out->push_back(']');
            break;
        }
        case JsonValueTypeObject: {
            out->push_back('{');
            const std::map<std::string, JsonValue*>* obj = JsonObjectImpl(value->object);
            bool first = true;
            for (auto& item : *obj) {
                if (!first) out->push_back(',');
                first = false;
                out->push_back('"');
                out->append(item.first);
                out->append("\":");
                SerializeValue(item.second, out);
            }
            out->push_back('}');
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// HLE handlers.  Guest addresses double as host pointers (direct-mapped guest
// memory), so args.arg1/arg2/... are reinterpreted directly.
// ---------------------------------------------------------------------------
u64 JsonMemAllocatorCtor(const GuestArgs& args) { return args.arg1; }

u64 JsonInitParameter2Ctor(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonInitParameter2*>(args.arg1);
    if (self != nullptr) std::memset(self, 0, sizeof(JsonInitParameter2));
    return args.arg1;
}

u64 JsonInitParameter2SetAllocator(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonInitParameter2*>(args.arg1);
    if (self != nullptr) {
        self->allocator = reinterpret_cast<void*>(args.arg2);
        self->user_data = reinterpret_cast<void*>(args.arg3);
    }
    return 0;
}

u64 JsonInitParameter2SetFileBufferSize(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonInitParameter2*>(args.arg1);
    if (self != nullptr) self->file_buffer_size = static_cast<size_t>(args.arg2);
    return 0;
}

u64 JsonInitParameter2SetSpecialFloatFormatType(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonInitParameter2*>(args.arg1);
    if (self != nullptr) self->special_float_format_type = static_cast<uint32_t>(args.arg2);
    return 0;
}

u64 JsonInitializerCtor(const GuestArgs& args) { return args.arg1; }
u64 JsonInitializerInitialize(const GuestArgs& args) { (void)args; return 0; }
u64 JsonInitializerInitializeV1(const GuestArgs& args) { (void)args; return 0; }
u64 JsonInitializerTerminate(const GuestArgs& args) { (void)args; return 0; }
u64 JsonInitializerDtor(const GuestArgs& args) { (void)args; return 0; }
u64 JsonMemAllocatorDtor(const GuestArgs& args) { (void)args; return 0; }

u64 JsonValueCtor(const GuestArgs& args) {
    JsonValueInit(reinterpret_cast<JsonValue*>(args.arg1));
    return args.arg1;
}

u64 JsonValueDtor(const GuestArgs& args) {
    JsonValueClear(reinterpret_cast<JsonValue*>(args.arg1));
    return 0;
}

u64 JsonValueBoolCtor(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    JsonValueInit(self);
    self->type    = JsonValueTypeBoolean;
    self->boolean = (args.arg2 != 0);
    return args.arg1;
}

u64 JsonValueIntCtor(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    JsonValueInit(self);
    self->type    = JsonValueTypeInteger;
    self->integer = static_cast<int64_t>(args.arg2);
    return args.arg1;
}

u64 JsonValueDoubleCtor(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    JsonValueInit(self);
    self->type = JsonValueTypeReal;
    const u64 bits = GetIncomingXmm0();
    self->real = *reinterpret_cast<const double*>(&bits);
    return args.arg1;
}

u64 JsonValueCStringCtor(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    JsonValueInit(self);
    self->type   = JsonValueTypeString;
    self->string = JsonStringNew(reinterpret_cast<const char*>(args.arg2));
    return args.arg1;
}

u64 JsonValueStringCtor(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    JsonValueInit(self);
    self->type   = JsonValueTypeString;
    self->string = JsonStringNew(*JsonStringImpl(reinterpret_cast<JsonString*>(args.arg2)));
    return args.arg1;
}

u64 JsonValueObjectCtor(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    JsonValueInit(self);
    self->type   = JsonValueTypeObject;
    self->object = JsonObjectNew();
    return args.arg1;
}

u64 JsonValueArrayCtor(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    JsonValueInit(self);
    self->type  = JsonValueTypeArray;
    self->array = JsonArrayNew();
    return args.arg1;
}

u64 JsonValueAssign(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    const auto* src = reinterpret_cast<const JsonValue*>(args.arg2);
    JsonValueCopy(self, src);
    return args.arg1;
}

u64 JsonValueReferArray(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    JsonValueInit(self);
    self->type  = JsonValueTypeArray;
    self->array = reinterpret_cast<JsonArray*>(args.arg2);
    return args.arg1;
}

u64 JsonValueReferObject(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    JsonValueInit(self);
    self->type   = JsonValueTypeObject;
    self->object = reinterpret_cast<JsonObject*>(args.arg2);
    return args.arg1;
}

u64 JsonValueIndexString(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    const std::string key = *JsonStringImpl(reinterpret_cast<JsonString*>(args.arg2));
    if (self->type == JsonValueTypeObject) {
        return reinterpret_cast<u64>(JsonObjectLookup(self->object, key, true));
    }
    if (self->type == JsonValueTypeArray) {
        size_t idx = static_cast<size_t>(std::strtoull(key.c_str(), nullptr, 10));
        auto* impl = JsonArrayImpl(self->array);
        if (idx < impl->size()) return reinterpret_cast<u64>((*impl)[idx]);
    }
    return 0;
}

u64 JsonValueIndexUInt(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    size_t idx = static_cast<size_t>(args.arg2);
    if (self->type == JsonValueTypeArray) {
        auto* impl = JsonArrayImpl(self->array);
        if (idx < impl->size()) return reinterpret_cast<u64>((*impl)[idx]);
    }
    return 0;
}

u64 JsonValueGetString(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    if (self != nullptr && self->type == JsonValueTypeString && self->string != nullptr) {
        return reinterpret_cast<u64>(self->string);
    }
    return 0;
}

u64 JsonValueGetBoolean(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    return (self != nullptr && self->type == JsonValueTypeBoolean && self->boolean) ? 1 : 0;
}

u64 JsonValueGetInteger(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    if (self != nullptr && self->type == JsonValueTypeInteger) return static_cast<u64>(self->integer);
    return 0;
}

u64 JsonValueGetReal(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    if (self != nullptr && self->type == JsonValueTypeReal) {
        u64 bits = 0;
        std::memcpy(&bits, &self->real, sizeof(bits));
        return bits;
    }
    return 0;
}

u64 JsonValueGetType(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    return (self != nullptr) ? self->type : 0;
}

u64 JsonValueGetObject(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    if (self != nullptr && self->type == JsonValueTypeObject) return reinterpret_cast<u64>(self->object);
    return 0;
}

u64 JsonValueGetArray(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    if (self != nullptr && self->type == JsonValueTypeArray) return reinterpret_cast<u64>(self->array);
    return 0;
}

u64 JsonValueCount(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    if (self == nullptr) return 0;
    switch (self->type) {
        case JsonValueTypeArray:  return JsonArrayImpl(self->array)->size();
        case JsonValueTypeObject: return JsonObjectImpl(self->object)->size();
        default: return 0;
    }
}

u64 JsonValueSerialize(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    JsonString* dst = reinterpret_cast<JsonString*>(args.arg2);
    std::string out;
    SerializeValue(self, &out);
    *JsonStringImpl(dst) = out;
    return 0;
}

u64 JsonValueSetBool(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    JsonValueClear(self);
    self->type    = JsonValueTypeBoolean;
    self->boolean = (args.arg2 != 0);
    return args.arg1;
}

u64 JsonValueSetInt(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    JsonValueClear(self);
    self->type    = JsonValueTypeInteger;
    self->integer = static_cast<int64_t>(args.arg2);
    return args.arg1;
}

u64 JsonValueSetUInt(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    JsonValueClear(self);
    self->type     = JsonValueTypeUnsigned;
    self->uinteger = args.arg2;
    return args.arg1;
}

u64 JsonValueSetDouble(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    JsonValueClear(self);
    self->type = JsonValueTypeReal;
    const u64 bits = GetIncomingXmm0();
    self->real = *reinterpret_cast<const double*>(&bits);
    return args.arg1;
}

u64 JsonValueSetString(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    JsonValueClear(self);
    self->type   = JsonValueTypeString;
    self->string = JsonStringNew(*JsonStringImpl(reinterpret_cast<JsonString*>(args.arg2)));
    return args.arg1;
}

u64 JsonValueSetType(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonValue*>(args.arg1);
    JsonValueClear(self);
    self->type = static_cast<uint32_t>(args.arg2);
    if (self->type == JsonValueTypeArray)  self->array  = JsonArrayNew();
    if (self->type == JsonValueTypeObject) self->object = JsonObjectNew();
    return args.arg1;
}

// ---- String ----
u64 JsonStringCtor(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonString*>(args.arg1);
    self->impl = new std::string;
    return args.arg1;
}

u64 JsonStringCStringCtor(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonString*>(args.arg1);
    self->impl = new std::string(reinterpret_cast<const char*>(args.arg2));
    return args.arg1;
}

u64 JsonStringDtor(const GuestArgs& args) {
    JsonStringDelete(reinterpret_cast<JsonString*>(args.arg1));
    return 0;
}

u64 JsonStringAssign(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonString*>(args.arg1);
    *JsonStringImpl(self) = *JsonStringImpl(reinterpret_cast<JsonString*>(args.arg2));
    return args.arg1;
}

u64 JsonStringCStr(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonString*>(args.arg1);
    return reinterpret_cast<u64>(JsonStringImpl(self)->c_str());
}

u64 JsonStringLength(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonString*>(args.arg1);
    return JsonStringImpl(self)->size();
}

// ---- Array ----
u64 JsonArrayCtor(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonArray*>(args.arg1);
    self->impl = new std::vector<JsonValue*>;
    return args.arg1;
}

u64 JsonArrayPushBack(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonArray*>(args.arg1);
    auto* val  = reinterpret_cast<JsonValue*>(args.arg2);
    JsonArrayImpl(self)->push_back(val);
    return args.arg1;
}

u64 JsonArrayBack(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonArray*>(args.arg1);
    auto* impl = JsonArrayImpl(self);
    if (impl != nullptr && !impl->empty()) return reinterpret_cast<u64>(impl->back());
    return 0;
}

u64 JsonArraySize(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonArray*>(args.arg1);
    return JsonArrayImpl(self)->size();
}

// ---- Object ----
u64 JsonObjectCtor(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonObject*>(args.arg1);
    self->impl = new std::map<std::string, JsonValue*>;
    return args.arg1;
}

u64 JsonObjectCopyCtor(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonObject*>(args.arg1);
    const auto* src = reinterpret_cast<const JsonObject*>(args.arg2);
    self->impl = new std::map<std::string, JsonValue*>;
    if (src->impl != nullptr) {
        for (auto& item : *src->impl) {
            JsonValue* nv = JsonValueNew();
            JsonValueCopy(nv, item.second);
            (*self->impl)[item.first] = nv;
        }
    }
    return args.arg1;
}

u64 JsonObjectDtor(const GuestArgs& args) {
    JsonObjectDelete(reinterpret_cast<JsonObject*>(args.arg1));
    return 0;
}

u64 JsonObjectAssign(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonObject*>(args.arg1);
    const auto* src = reinterpret_cast<const JsonObject*>(args.arg2);
    JsonObjectDelete(self);
    self->impl = new std::map<std::string, JsonValue*>;
    if (src->impl != nullptr) {
        for (auto& item : *src->impl) {
            JsonValue* nv = JsonValueNew();
            JsonValueCopy(nv, item.second);
            (*self->impl)[item.first] = nv;
        }
    }
    return args.arg1;
}

u64 JsonObjectIndex(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonObject*>(args.arg1);
    const std::string key = *JsonStringImpl(reinterpret_cast<JsonString*>(args.arg2));
    return reinterpret_cast<u64>(JsonObjectLookup(self, key, true));
}

u64 JsonObjectClear(const GuestArgs& args) {
    auto* self = reinterpret_cast<JsonObject*>(args.arg1);
    auto* impl = JsonObjectImpl(self);
    if (impl != nullptr) {
        for (auto& item : *impl) JsonValueDelete(item.second);
        impl->clear();
    }
    return 0;
}

// ---- Parser ----
u64 JsonParserParse(const GuestArgs& args) {
    auto* dst = reinterpret_cast<JsonValue*>(args.arg1);
    const char* src = reinterpret_cast<const char*>(args.arg2);
    size_t size = static_cast<size_t>(args.arg3);
    if (dst == nullptr || src == nullptr) return static_cast<u64>(JSON_ERROR_INVALID_ARGUMENT);
    JsonParserState st { src, src + size };
    JsonValue parsed;
    JsonValueInit(&parsed);
    if (!ParseValue(st, &parsed)) {
        JsonValueClear(&parsed);
        return static_cast<u64>(JSON_ERROR_PARSE_INVALID_CHAR);
    }
    JsonValueCopy(dst, &parsed);
    JsonValueClear(&parsed);
    return 0;
}

u64 JsonInitializerSetGlobalNullAccessCallback(const GuestArgs& args) { (void)args; return 0; }

} // anonymous namespace

void RegisterLibJson() {
    LOG_INFO(HLE, "Registering libSceJson HLE symbols...");

    const char* module = "libSceJson";

    RegisterSymbol(module, "-hJRce8wn1U", JsonMemAllocatorCtor);
    RegisterSymbol(module, "OcAgPxcq5Vk", JsonMemAllocatorDtor);
    RegisterSymbol(module, "WSOuge5IsCg", JsonInitParameter2Ctor);
    RegisterSymbol(module, "GvGvswb0v34", JsonInitParameter2Ctor);
    RegisterSymbol(module, "I2QC8PYhJWY", JsonInitParameter2SetAllocator);
    RegisterSymbol(module, "W72B9ylU2JA", JsonInitParameter2SetAllocator);
    RegisterSymbol(module, "Eu95jmqn5Rw", JsonInitParameter2SetFileBufferSize);
    RegisterSymbol(module, "WVZBP4IyM+E", JsonInitParameter2SetSpecialFloatFormatType);
    RegisterSymbol(module, "cK6bYHf-Q5E", JsonInitializerCtor);
    RegisterSymbol(module, "IXW-z8pggfg", JsonInitializerInitialize);
    RegisterSymbol(module, "Cxwy7wHq4J0", JsonInitializerInitializeV1);
    RegisterSymbol(module, "PR5k1penBLM", JsonInitializerTerminate);
    RegisterSymbol(module, "RujUxbr3haM", JsonInitializerDtor);
    RegisterSymbol(module, "qBMjqyBn3OM", JsonValueCtor);          // Value()
    RegisterSymbol(module, "-wa17B7TGnw", JsonValueCtor);
    RegisterSymbol(module, "WTtYf+cNnXI", JsonValueDtor);          // ~Value()
    RegisterSymbol(module, "0eUrW9JAxM0", JsonValueDtor);
    RegisterSymbol(module, "S5JxQnoGF3E", JsonParserParse);
    RegisterSymbol(module, "HwDt5lD9Bfo", JsonValueIndexString);
    RegisterSymbol(module, "epJ6x2LV0kU", JsonValueGetString);
    RegisterSymbol(module, "L1KAkYWml-M", JsonStringCStr);
    RegisterSymbol(module, "OJPTonqdg0I", JsonObjectCtor);
    RegisterSymbol(module, "sZIoMRGO+jk", JsonValueStringCtor);
    RegisterSymbol(module, "ERuf9y0DY84", JsonObjectIndex);
    RegisterSymbol(module, "4zrm6VrgIAw", JsonValueAssign);
    RegisterSymbol(module, "a+W7HHlwpBs", JsonObjectCopyCtor);
    RegisterSymbol(module, "5JmzZt8twAo", JsonObjectDtor);
    RegisterSymbol(module, "nM5XqdeXFPw", JsonValueReferArray);
    RegisterSymbol(module, "-NxEk7XLkDY", JsonValueReferObject);
    RegisterSymbol(module, "zQtLRTqceMY", JsonArrayPushBack);
    RegisterSymbol(module, "0lLK8+kDqmE", JsonValueIntCtor);
    RegisterSymbol(module, "urOpESTBZmo", JsonObjectAssign);
    RegisterSymbol(module, "zTwZdI8AZ5Y", JsonValueGetBoolean);
    RegisterSymbol(module, "R7FDWtcN6f8", JsonValueSerialize);
    RegisterSymbol(module, "oH8aBmLU+fc", JsonObjectClear);
    RegisterSymbol(module, "bAM9Qwofus0", JsonArrayBack);
    RegisterSymbol(module, "UeuWT+yNdCQ", JsonValueBoolCtor);
    RegisterSymbol(module, "3xUXnmUkXfo", JsonValueObjectCtor);
    RegisterSymbol(module, "cn9svYGWKDQ", JsonStringAssign);
    RegisterSymbol(module, "b9V6fmppLXY", JsonValueCStringCtor);
    RegisterSymbol(module, "EUH+EmT-v9E", JsonStringLength);
    RegisterSymbol(module, "XlWbvieLj2M", JsonValueIndexUInt);
    RegisterSymbol(module, "IlsmvBtMkak", JsonValueGetObject);
    RegisterSymbol(module, "SHtAad20YYM", JsonValueGetType);
    RegisterSymbol(module, "DIxvoy7Ngvk", JsonValueGetInteger);
    RegisterSymbol(module, "qSmqLXXCPas", JsonStringCtor);
    RegisterSymbol(module, "ONT8As5R1ug", JsonValueGetArray);
    RegisterSymbol(module, "3qrge7L-AU4", JsonValueGetReal);
    RegisterSymbol(module, "sOmU4vnx3s0", JsonValueDoubleCtor);
    RegisterSymbol(module, "rQGJeNjOuUk", JsonArraySize);
    RegisterSymbol(module, "5yHuiWXo2gg", JsonValueSetBool);
    RegisterSymbol(module, "QxVVYhP-mvg", JsonValueSetInt);
    RegisterSymbol(module, "SIe1ZmW7e7s", JsonValueSetUInt);
    RegisterSymbol(module, "BSmWDIkV4w4", JsonValueSetDouble);
    RegisterSymbol(module, "6l3Bv2gysNc", JsonValueSetString);
    RegisterSymbol(module, "9KUZFjI1IxA", JsonStringCStringCtor);
    RegisterSymbol(module, "cG1VE2HMl6c", JsonStringDtor);
    RegisterSymbol(module, "IKQimvG9Wqs", JsonValueSetType);
    RegisterSymbol(module, "RBw+4NukeGQ", JsonValueCount);
    RegisterSymbol(module, "+drDFyAS6u4", JsonInitializerSetGlobalNullAccessCallback);
    RegisterSymbol(module, "00oCq0RwSAY", JsonInitializerSetGlobalNullAccessCallback);

    LOG_INFO(HLE, "libSceJson HLE symbols registered successfully.");
}

} // namespace HLE
