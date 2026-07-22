#include "libjson.h"
#include "hle.h"
#include "../memory/memory.h"
#include "../common/log.h"

#include <cstring>
#include <unordered_map>
#include <mutex>
#include <string>

namespace HLE {
namespace {

// sce::Json::Value type tags (from Orbis SDK)
enum JsonValueType : u32 {
    kNull     = 0,
    kBoolean  = 1,
    kInteger  = 2,
    kUnsigned = 3,
    kReal     = 4,
    kString   = 5,
    kArray    = 6,
    kObject   = 7,
};

// Write a sce::Json::Value variant at the guest address with the given type
// and 64-bit data payload.  The Value is 24 bytes:
//   [0..3] type  (u32)
//   [4..7] flags (u32, zeroed)
//   [8..15] data (union)
//   [16..23] size/capacity (used by string/array/object, zeroed for scalars)
void WriteJsonValue(guest_addr_t addr, JsonValueType type, u64 data, u64 size = 0) {
    Memory::Write<u32>(addr,     static_cast<u32>(type));
    Memory::Write<u32>(addr + 4, 0);
    Memory::Write<u64>(addr + 8, data);
    Memory::Write<u64>(addr + 16, size);
}

// ---- sce::Json::Value constructors ----

// Value() default constructor — sets type to Null.
u64 ValueDefaultConstructor(const GuestArgs& args) {
    WriteJsonValue(args.arg1, kNull, 0);
    LOG_DEBUG(HLE, "sce::Json::Value() default @ 0x%llx", args.arg1);
    return args.arg1;
}

// Value(bool b) — sets type to Boolean.
u64 ValueBooleanConstructor(const GuestArgs& args) {
    WriteJsonValue(args.arg1, kBoolean, args.arg2 & 0xFF);
    LOG_DEBUG(HLE, "sce::Json::Value(bool %llu) @ 0x%llx", args.arg2 & 0xFF, args.arg1);
    return args.arg1;
}

// Value(long i) — sets type to Integer.
u64 ValueIntegerConstructor(const GuestArgs& args) {
    WriteJsonValue(args.arg1, kInteger, static_cast<u64>(static_cast<s64>(args.arg2)));
    LOG_DEBUG(HLE, "sce::Json::Value(int %lld) @ 0x%llx", static_cast<s64>(args.arg2), args.arg1);
    return args.arg1;
}

// Value(unsigned long u) — sets type to Unsigned.
u64 ValueUnsignedConstructor(const GuestArgs& args) {
    WriteJsonValue(args.arg1, kUnsigned, args.arg2);
    LOG_DEBUG(HLE, "sce::Json::Value(unsigned %llu) @ 0x%llx", args.arg2, args.arg1);
    return args.arg1;
}

// Value(double d) — real constructor.  The double comes from XMM0 (SysV ABI).
// Our dispatcher saves XMM0 before the call and exposes it via
// GetIncomingXmm0().
u64 ValueRealConstructor(const GuestArgs& args) {
    const u64 bits = GetIncomingXmm0();
    WriteJsonValue(args.arg1, kReal, bits);
    LOG_DEBUG(HLE, "sce::Json::Value(double 0x%llx = %g) @ 0x%llx", bits,
              *reinterpret_cast<const double*>(&bits), args.arg1);
    return args.arg1;
}

// Value(const char* s) — string from C string pointer.
u64 ValueCStringConstructor(const GuestArgs& args) {
    guest_addr_t str_ptr = args.arg2;
    if (str_ptr) {
        u64 len = 0;
        while (Memory::Read<u8>(str_ptr + len) != 0) ++len;
        WriteJsonValue(args.arg1, kString, str_ptr, len);
    } else {
        WriteJsonValue(args.arg1, kString, 0, 0);
    }
    LOG_DEBUG(HLE, "sce::Json::Value(cstr @ 0x%llx) @ 0x%llx", str_ptr, args.arg1);
    return args.arg1;
}

// ~Value() — destructor (no-op, the managed heap handles cleanup).
u64 ValueDestructor(const GuestArgs& args) {
    LOG_DEBUG(HLE, "sce::Json::Value dtor @ 0x%llx", args.arg1);
    // Nothing to free — the guest owns the Value's inline storage.
    return 0;
}

// ---- sce::Json::Value setters ----

// set(bool b)
u64 ValueSetBoolean(const GuestArgs& args) {
    WriteJsonValue(args.arg1, kBoolean, args.arg2 & 0xFF);
    return args.arg1;
}

// set(long i)
u64 ValueSetInteger(const GuestArgs& args) {
    WriteJsonValue(args.arg1, kInteger, static_cast<u64>(static_cast<s64>(args.arg2)));
    return args.arg1;
}

// set(unsigned long u)
u64 ValueSetUnsigned(const GuestArgs& args) {
    WriteJsonValue(args.arg1, kUnsigned, args.arg2);
    return args.arg1;
}

// set(double d) — double from XMM0 via GetIncomingXmm0().
u64 ValueSetReal(const GuestArgs& args) {
    const u64 bits = GetIncomingXmm0();
    WriteJsonValue(args.arg1, kReal, bits);
    LOG_DEBUG(HLE, "sce::Json::Value::set(double 0x%llx = %g) @ 0x%llx", bits,
              *reinterpret_cast<const double*>(&bits), args.arg1);
    return args.arg1;
}

// set(Type t) — explicit type (no value change).
u64 ValueSetType(const GuestArgs& args) {
    const u32 t = static_cast<u32>(args.arg2 & 0xFF);
    Memory::Write<u32>(args.arg1, t);
    return args.arg1;
}

// set(const char* s) — string from C string pointer.
u64 ValueSetCString(const GuestArgs& args) {
    guest_addr_t str_ptr = args.arg2;
    if (str_ptr) {
        u64 len = 0;
        while (Memory::Read<u8>(str_ptr + len) != 0) ++len;
        WriteJsonValue(args.arg1, kString, str_ptr, len);
    } else {
        WriteJsonValue(args.arg1, kString, 0, 0);
    }
    return args.arg1;
}

// clear() — reset to Null.
u64 ValueClear(const GuestArgs& args) {
    WriteJsonValue(args.arg1, kNull, 0);
    return args.arg1;
}

// ---- sce::Json::String ----

// String(const char* s)
u64 StringCStringConstructor(const GuestArgs& args) {
    guest_addr_t str_ptr = args.arg2;
    u64 len = 0;
    if (str_ptr) {
        while (Memory::Read<u8>(str_ptr + len) != 0) ++len;
    }
    // String layout: [0..7] = ptr, [8..15] = length
    Memory::Write<u64>(args.arg1, str_ptr);
    Memory::Write<u64>(args.arg1 + 8, len);
    LOG_DEBUG(HLE, "sce::Json::String(cstr @ 0x%llx, len=%llu) @ 0x%llx", str_ptr, len, args.arg1);
    return args.arg1;
}

// String() — default constructor, empty string.
u64 StringDefaultConstructor(const GuestArgs& args) {
    Memory::Write<u64>(args.arg1, 0);
    Memory::Write<u64>(args.arg1 + 8, 0);
    return args.arg1;
}

// ~String() — destructor (no-op).
u64 StringDestructor(const GuestArgs& args) {
    LOG_DEBUG(HLE, "sce::Json::String dtor @ 0x%llx", args.arg1);
    return 0;
}

} // anonymous namespace

void RegisterLibJson() {
    LOG_INFO(HLE, "Registering libSceJson HLE symbols...");

    const char* module = "libSceJson";

    // sce::Json::Value constructors
    RegisterSymbol(module, "qBMjqyBn3OM", ValueDefaultConstructor);  // Value()
    RegisterSymbol(module, "UeuWT+yNdCQ", ValueBooleanConstructor);  // Value(bool)
    RegisterSymbol(module, "0lLK8+kDqmE", ValueIntegerConstructor);  // Value(long)
    RegisterSymbol(module, "x4AUdbhpRB0", ValueUnsignedConstructor); // Value(unsigned long)
    RegisterSymbol(module, "sOmU4vnx3s0", ValueRealConstructor);     // Value(double)
    RegisterSymbol(module, "b9V6fmppLXY", ValueCStringConstructor);  // Value(const char*)
    RegisterSymbol(module, "CbrT3dwDILo", ValueSetType);             // Value(ValueType)
    RegisterSymbol(module, "sZIoMRGO+jk", ValueSetType);             // Value(const String&) — re-use set-type

    // sce::Json::Value destructor
    RegisterSymbol(module, "WTtYf+cNnXI", ValueDestructor);          // ~Value()

    // sce::Json::Value setters
    RegisterSymbol(module, "5yHuiWXo2gg", ValueSetBoolean);          // set(bool)
    RegisterSymbol(module, "QxVVYhP-mvg", ValueSetInteger);          // set(long)
    RegisterSymbol(module, "SIe1ZmW7e7s", ValueSetUnsigned);         // set(unsigned long)
    RegisterSymbol(module, "BSmWDIkV4w4", ValueSetReal);             // set(double)
    RegisterSymbol(module, "IKQimvG9Wqs", ValueSetType);             // set(ValueType)
    RegisterSymbol(module, "n6FC+l9DU70", ValueSetCString);          // set(const char*)
    RegisterSymbol(module, "6l3Bv2gysNc", ValueSetCString);          // set(const String&)
    RegisterSymbol(module, "FIjXN2TkuTs", ValueClear);               // clear()

    // sce::Json::String constructors / destructor
    RegisterSymbol(module, "9KUZFjI1IxA", StringCStringConstructor); // String(const char*)
    RegisterSymbol(module, "qSmqLXXCPas", StringDefaultConstructor); // String()
    RegisterSymbol(module, "0CAesfH963Q", StringCStringConstructor); // String(const String&) — copy ctor
    RegisterSymbol(module, "cG1VE2HMl6c", StringDestructor);         // ~String()

    LOG_INFO(HLE, "libSceJson HLE symbols registered successfully.");
}

} // namespace HLE
