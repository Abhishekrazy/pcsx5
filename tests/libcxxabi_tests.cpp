// Unit tests for the guest C++ ABI / unwind HLE surface (src/hle/liblibc.cpp).
//
// Covered (via HleDispatch against the registered symbols):
//   - __cxa_allocate_exception / __cxa_free_exception: libc++abi header layout
//     (referenceCount, zeroed fields, 16-byte alignment of the user pointer)
//   - __cxa_begin_catch / __cxa_end_catch bookkeeping: handlerCount,
//     adjustedPtr return value, __cxa_current_exception_type
//   - __cxa_get_exception_ptr
//   - __cxa_guard_acquire / release: static-init interlock byte semantics
//   - _Unwind_GetGR / SetGR / GetIP / SetIP / GetLanguageSpecificData over a
//     fabricated unwind context (magic-tagged)
//   - __cxa_get_globals returns a stable per-thread pointer
//   - __gxx_personality_v0 (HLE fallback) returns _URC_CONTINUE_UNWIND
//
// The full DWARF unwind path (phase 1/2 over guest .eh_frame with a guest
// personality) cannot run in a unit test: it needs executable guest code for
// the personality and landing pads.  It is exercised end-to-end by booting a
// title that throws (Dreaming Sarah content-load).
//
// Build target: libcxxabi_tests (see CMakeLists.txt).

#include "hle/hle.h"
#include "memory/memory.h"

#include <cstdio>
#include <cstring>
#include <string>

extern "C" u64 HleDispatch(u64, u64, u64, u64, u64, u64, u64, u64, u64);

namespace {

int g_failures = 0;

#define EXPECT(cond, msg)                                                        \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

// Read the symbol_id encoded in a thunk (see tests/hle_import_report.cpp).
u64 ReadSymbolIdFromThunk(guest_addr_t thunk_addr) {
    if (!thunk_addr) return 0;
    u8 buf[10] = {};
    std::memcpy(buf, reinterpret_cast<const void*>(thunk_addr), sizeof(buf));
    u64 id = 0;
    for (int i = 0; i < 8; ++i) {
        id |= static_cast<u64>(buf[2 + i]) << (8 * i);
    }
    return id;
}

u64 ResolveId(const char* name) {
    const u64 id = ReadSymbolIdFromThunk(HLE::Resolve("libSceLibcInternal", name));
    if (id == 0) {
        std::fprintf(stderr, "[FAIL] %s does not resolve\n", name);
        ++g_failures;
    }
    return id;
}

// libc++abi __cxa_exception layout offsets (from user pointer).
constexpr s64 kHdrFromUser    = 120; // header starts 120 bytes before user ptr
constexpr s64 kUeFromUser     = 32;  // _Unwind_Exception is the last member
constexpr u64 kCppClass       = 0x474E5543432B2B00ULL; // "GNUCC++\0"
constexpr u64 kUnwindCtxMagic = 0x5043583555584358ULL;

u64 Read64(u64 addr) {
    u64 v = 0;
    std::memcpy(&v, reinterpret_cast<const void*>(addr), 8);
    return v;
}
u32 Read32(u64 addr) {
    u32 v = 0;
    std::memcpy(&v, reinterpret_cast<const void*>(addr), 4);
    return v;
}
void Write64(u64 addr, u64 v) {
    std::memcpy(reinterpret_cast<void*>(addr), &v, 8);
}
void Write32(u64 addr, u32 v) {
    std::memcpy(reinterpret_cast<void*>(addr), &v, 4);
}

void TestAllocateFree() {
    const u64 alloc_id = ResolveId("__cxa_allocate_exception");
    const u64 free_id  = ResolveId("__cxa_free_exception");
    if (!alloc_id || !free_id) return;

    const u64 user = HleDispatch(alloc_id, 64, 0, 0, 0, 0, 0, 0x1000, 0);
    EXPECT(user != 0, "__cxa_allocate_exception(64) returns non-null");
    if (!user) return;
    EXPECT((user & 0xF) == 0, "user pointer is 16-byte aligned");
    const u64 hdr = user - kHdrFromUser;
    EXPECT(Read64(hdr + 0) == 1, "referenceCount initialized to 1");
    EXPECT(Read64(hdr + 8) == 0, "exceptionType zeroed");
    EXPECT(Read64(hdr + 16) == 0, "exceptionDestructor zeroed");
    EXPECT(Read32(hdr + 48) == 0, "handlerCount zeroed");
    EXPECT(Read64(hdr + 88) == 0, "unwindHeader.exception_class zeroed");

    // A second allocation must not overlap the first.
    const u64 user2 = HleDispatch(alloc_id, 64, 0, 0, 0, 0, 0, 0x1001, 0);
    EXPECT(user2 != 0 && user2 != user, "second allocation is distinct");

    HleDispatch(free_id, user, 0, 0, 0, 0, 0, 0x1002, 0);
    HleDispatch(free_id, user2, 0, 0, 0, 0, 0, 0x1003, 0);
    EXPECT(true, "free does not crash");
}

// Builds a thrown-exception header the way __cxa_throw would (we cannot call
// __cxa_throw itself in a unit test: with no .eh_frame registered it takes
// the std::terminate path and exits the process).
u64 FabricateThrownException(u64 alloc_id, u64 tinfo) {
    const u64 user = HleDispatch(alloc_id, 64, 0, 0, 0, 0, 0, 0x1100, 0);
    if (!user) return 0;
    const u64 hdr = user - kHdrFromUser;
    Write64(hdr + 8, tinfo);          // exceptionType
    Write64(hdr + 80, user);          // adjustedPtr
    Write64(user - kUeFromUser, kCppClass); // unwindHeader.exception_class
    return user;
}

void TestBeginEndCatch() {
    const u64 alloc_id   = ResolveId("__cxa_allocate_exception");
    const u64 begin_id   = ResolveId("__cxa_begin_catch");
    const u64 end_id     = ResolveId("__cxa_end_catch");
    const u64 tinfo_id   = ResolveId("__cxa_current_exception_type");
    const u64 getptr_id  = ResolveId("__cxa_get_exception_ptr");
    if (!alloc_id || !begin_id || !end_id || !tinfo_id || !getptr_id) return;

    const u64 tinfo = 0xDEADBEEF00;
    const u64 user = FabricateThrownException(alloc_id, tinfo);
    EXPECT(user != 0, "fabricated thrown exception");
    if (!user) return;
    const u64 ue = user - kUeFromUser;

    const u64 got_user = HleDispatch(begin_id, ue, 0, 0, 0, 0, 0, 0x1101, 0);
    EXPECT(got_user == user, "__cxa_begin_catch returns the user object pointer");
    const u64 hdr = user - kHdrFromUser;
    EXPECT(Read32(hdr + 48) == 1, "handlerCount incremented to 1");

    const u64 got_tinfo = HleDispatch(tinfo_id, 0, 0, 0, 0, 0, 0, 0x1102, 0);
    EXPECT(got_tinfo == tinfo, "__cxa_current_exception_type returns the tinfo");

    const u64 got_ptr = HleDispatch(getptr_id, ue, 0, 0, 0, 0, 0, 0x1103, 0);
    EXPECT(got_ptr == user, "__cxa_get_exception_ptr returns adjustedPtr");

    HleDispatch(end_id, 0, 0, 0, 0, 0, 0, 0x1104, 0);
    const u64 tinfo_after = HleDispatch(tinfo_id, 0, 0, 0, 0, 0, 0, 0x1105, 0);
    EXPECT(tinfo_after == 0, "caught stack empty after __cxa_end_catch");
}

void TestGuardVariables() {
    const u64 acq_id = ResolveId("__cxa_guard_acquire");
    const u64 rel_id = ResolveId("__cxa_guard_release");
    const u64 abt_id = ResolveId("__cxa_guard_abort");
    if (!acq_id || !rel_id || !abt_id) return;

    alignas(8) u64 guard = 0;
    const u64 g = reinterpret_cast<u64>(&guard);

    const u64 first = HleDispatch(acq_id, g, 0, 0, 0, 0, 0, 0x1200, 0);
    EXPECT(first == 1, "first __cxa_guard_acquire returns 1 (needs init)");
    HleDispatch(rel_id, g, 0, 0, 0, 0, 0, 0x1201, 0);
    EXPECT((guard & 0xFF) == 1, "__cxa_guard_release sets the initialized byte");
    const u64 second = HleDispatch(acq_id, g, 0, 0, 0, 0, 0, 0x1202, 0);
    EXPECT(second == 0, "__cxa_guard_acquire returns 0 once initialized");

    // Abort path resets the guard byte so the init can be retried.
    alignas(8) u64 guard2 = 0;
    const u64 g2 = reinterpret_cast<u64>(&guard2);
    EXPECT(HleDispatch(acq_id, g2, 0, 0, 0, 0, 0, 0x1203, 0) == 1,
           "acquire on fresh guard returns 1");
    HleDispatch(abt_id, g2, 0, 0, 0, 0, 0, 0x1204, 0);
    EXPECT((guard2 & 0xFF) == 0, "__cxa_guard_abort leaves guard byte 0");
    EXPECT(HleDispatch(acq_id, g2, 0, 0, 0, 0, 0, 0x1205, 0) == 1,
           "guard can be re-acquired after abort");
    HleDispatch(rel_id, g2, 0, 0, 0, 0, 0, 0x1206, 0);
}

void TestUnwindContextAccessors() {
    const u64 getgr_id = ResolveId("_Unwind_GetGR");
    const u64 setgr_id = ResolveId("_Unwind_SetGR");
    const u64 getip_id = ResolveId("_Unwind_GetIP");
    const u64 setip_id = ResolveId("_Unwind_SetIP");
    const u64 lsda_id  = ResolveId("_Unwind_GetLanguageSpecificData");
    const u64 rs_id    = ResolveId("_Unwind_GetRegionStart");
    if (!getgr_id || !setgr_id || !getip_id || !setip_id || !lsda_id || !rs_id) return;

    // Fabricated GuestUnwindContext: magic, gr[17], cfa, lsda, region_start.
    struct FakeCtx {
        u64 magic;
        u64 gr[17];
        u64 cfa;
        u64 lsda;
        u64 region_start;
    } ctx = {};
    ctx.magic = kUnwindCtxMagic;
    ctx.lsda = 0xABCDEF;
    ctx.region_start = 0x500000;
    const u64 cp = reinterpret_cast<u64>(&ctx);

    HleDispatch(setgr_id, cp, 3, 0x1234, 0, 0, 0, 0x1300, 0);
    EXPECT(HleDispatch(getgr_id, cp, 3, 0, 0, 0, 0, 0x1301, 0) == 0x1234,
           "_Unwind_SetGR/GetGR round-trip (rbx)");
    HleDispatch(setip_id, cp, 0x777000, 0, 0, 0, 0, 0x1302, 0);
    EXPECT(HleDispatch(getip_id, cp, 0, 0, 0, 0, 0, 0x1303, 0) == 0x777000,
           "_Unwind_SetIP/GetIP round-trip");
    EXPECT(HleDispatch(lsda_id, cp, 0, 0, 0, 0, 0, 0x1304, 0) == 0xABCDEF,
           "_Unwind_GetLanguageSpecificData returns lsda");
    EXPECT(HleDispatch(rs_id, cp, 0, 0, 0, 0, 0, 0x1305, 0) == 0x500000,
           "_Unwind_GetRegionStart returns region start");

    // Out-of-range register index and foreign context are rejected, not fatal.
    EXPECT(HleDispatch(getgr_id, cp, 42, 0, 0, 0, 0, 0x1306, 0) == 0,
           "_Unwind_GetGR with bad index returns 0");
    u64 foreign[4] = {};
    EXPECT(HleDispatch(getgr_id, reinterpret_cast<u64>(foreign), 0, 0, 0, 0, 0, 0x1307, 0) == 0,
           "_Unwind_GetGR with foreign context returns 0");
}

void TestEhGlobalsAndPersonality() {
    const u64 globals_id = ResolveId("__cxa_get_globals");
    const u64 pers_id    = ResolveId("__gxx_personality_v0");
    if (!globals_id || !pers_id) return;

    const u64 g1 = HleDispatch(globals_id, 0, 0, 0, 0, 0, 0, 0x1400, 0);
    const u64 g2 = HleDispatch(globals_id, 0, 0, 0, 0, 0, 0, 0x1401, 0);
    EXPECT(g1 != 0 && g1 == g2, "__cxa_get_globals returns a stable per-thread pointer");

    // Search phase (actions=1) and cleanup phase (actions=2) both report
    // _URC_CONTINUE_UNWIND (8) from the HLE fallback personality.
    EXPECT(HleDispatch(pers_id, 1, 1, kCppClass, 0, 0, 0, 0x1402, 0) == 8,
           "HLE personality: search phase -> CONTINUE_UNWIND");
    EXPECT(HleDispatch(pers_id, 1, 2, kCppClass, 0, 0, 0, 0x1403, 0) == 8,
           "HLE personality: cleanup phase -> CONTINUE_UNWIND");
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    if (!Memory::Initialize()) {
        std::fprintf(stderr, "FATAL: Memory::Initialize failed\n");
        return 2;
    }
    if (!HLE::Initialize()) {
        std::fprintf(stderr, "FATAL: HLE::Initialize failed\n");
        Memory::Shutdown();
        return 2;
    }
    HLE::SetStrictImportMode(false);

    TestAllocateFree();
    TestBeginEndCatch();
    TestGuardVariables();
    TestUnwindContextAccessors();
    TestEhGlobalsAndPersonality();

    HLE::Shutdown();
    Memory::Shutdown();

    if (g_failures == 0) {
        std::fprintf(stdout, "libc C++ ABI tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "libc C++ ABI tests: %d failure(s).\n", g_failures);
    return 1;
}
