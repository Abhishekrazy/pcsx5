// Unit tests for the Phase-3 HLE modules: libSceSysmodule (fake id registry),
// the libc guest heap (malloc/free/calloc/realloc/memalign/posix_memalign),
// libSceSystemService canned parameters, the NP offline-signed-in set, the
// libSceVideoOut port/flip model with equeue notifications, and the
// NID-database gap-filler stubs.
//
// Build target: hle_phase3_tests (see CMakeLists.txt).

#include "hle/hle.h"
#include "hle/libkernel_sync.h"
#include "memory/memory.h"
#include "common/log.h"
#include "common/nid.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" u64 HleDispatch(u64, u64, u64, u64, u64, u64, u64, u64, u64);

namespace {

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

// Resolve + return the dispatchable symbol id for a registered symbol.
u64 SymbolId(const char* module, const char* name) {
    return ReadSymbolIdFromThunk(HLE::Resolve(module, name));
}

u64 SymbolIdAny(const char* name) {
    return ReadSymbolIdFromThunk(HLE::ResolveAny(name));
}

int g_failures = 0;

#define EXPECT(cond, msg)                                                        \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

#define EXPECT_EQ(a, b, msg)                                                                   \
    do {                                                                                       \
        auto _lhs = (a);                                                                       \
        auto _rhs = (b);                                                                       \
        if (!(_lhs == _rhs)) {                                                                 \
            std::fprintf(stderr, "[FAIL] %s:%d: %s  (lhs=%lld rhs=%lld)\n",                    \
                         __FILE__, __LINE__, msg,                                              \
                         (long long)_lhs, (long long)_rhs);                                    \
            ++g_failures;                                                                      \
        }                                                                                      \
    } while (0)

// ---------------------------------------------------------------------------
// libSceSysmodule — fake id-registry bookkeeping.
// ---------------------------------------------------------------------------
void TestSysmodule() {
    std::fprintf(stdout, "[TEST] libSceSysmodule id registry\n");

    const u64 load_id     = SymbolId("libSceSysmodule", "sceSysmoduleLoadModule");
    const u64 isloaded_id = SymbolId("libSceSysmodule", "sceSysmoduleIsLoaded");
    const u64 unload_id   = SymbolId("libSceSysmodule", "sceSysmoduleUnloadModule");
    EXPECT(load_id && isloaded_id && unload_id, "sysmodule symbols resolve");

    // NID-form resolution also works (cross-module).
    EXPECT(HLE::ResolveAny("g8cM39EUZ6o#T#T") != 0, "sceSysmoduleLoadModule NID resolves");

    EXPECT_EQ(HleDispatch(load_id, 0x0006, 0, 0, 0, 0, 0, 0x1000, 0), (u64)0,
              "LoadModule(libSceFiber) -> 0");
    EXPECT_EQ(HleDispatch(isloaded_id, 0x0006, 0, 0, 0, 0, 0, 0x1001, 0), (u64)0,
              "IsLoaded(0x0006) -> 0 after load");
    EXPECT_EQ(HleDispatch(unload_id, 0x0006, 0, 0, 0, 0, 0, 0x1002, 0), (u64)0,
              "UnloadModule(0x0006) -> 0");
    EXPECT_EQ(HleDispatch(isloaded_id, 0x0006, 0, 0, 0, 0, 0, 0x1003, 0), (u64)0x80020002,
              "IsLoaded(0x0006) -> 0x80020002 after unload");
    // Unknown id: registry miss -> NOT_FOUND, matching SharpEmu semantics.
    EXPECT_EQ(HleDispatch(isloaded_id, 0x0BAD, 0, 0, 0, 0, 0, 0x1004, 0), (u64)0x80020002,
              "IsLoaded(unknown) -> 0x80020002");

    // libkernel-scoped NID aliases now point at the real implementation.
    EXPECT_EQ(HleDispatch(load_id, 0x0006, 0, 0, 0, 0, 0, 0x1005, 0), (u64)0,
              "LoadModule(0x0006) reload -> 0");
    EXPECT_EQ(HleDispatch(SymbolId("libkernel", "fMP5NHUOaMk#D#E"), 0x0006, 0, 0, 0, 0, 0, 0x1006, 0),
              (u64)0, "IsLoaded via libkernel NID -> 0");
}

// ---------------------------------------------------------------------------
// libc guest heap — malloc/free/calloc/realloc/memalign/posix_memalign.
// ---------------------------------------------------------------------------
void TestGuestHeap() {
    std::fprintf(stdout, "[TEST] libc guest heap\n");

    const u64 malloc_id  = SymbolId("libc", "malloc");
    const u64 free_id    = SymbolId("libc", "free");
    const u64 calloc_id  = SymbolId("libc", "calloc");
    const u64 realloc_id = SymbolId("libc", "realloc");
    EXPECT(malloc_id && free_id && calloc_id && realloc_id, "heap symbols resolve");

    // malloc + read/write through the returned guest pointer.
    const u64 p = HleDispatch(malloc_id, 100, 0, 0, 0, 0, 0, 0x2000, 0);
    EXPECT(p != 0, "malloc(100) returns non-null");
    std::memset(reinterpret_cast<void*>(p), 0x5A, 100);
    EXPECT(reinterpret_cast<const u8*>(p)[99] == 0x5A, "malloc block is writable");

    // The libkernel NID registration routes to the same heap now.
    const u64 p2 = HleDispatch(SymbolIdAny("gQX+4GDQjpM#T#T"), 64, 0, 0, 0, 0, 0, 0x2001, 0);
    EXPECT(p2 != 0, "malloc via libkernel NID returns non-null");

    // realloc preserves contents across a size increase.
    const u64 r0 = HleDispatch(malloc_id, 64, 0, 0, 0, 0, 0, 0x2002, 0);
    EXPECT(r0 != 0, "malloc(64)");
    for (int i = 0; i < 64; ++i) reinterpret_cast<u8*>(r0)[i] = static_cast<u8>(i);
    const u64 r1 = HleDispatch(realloc_id, r0, 4096, 0, 0, 0, 0, 0x2003, 0);
    EXPECT(r1 != 0, "realloc to 4096 returns non-null");
    bool preserved = true;
    for (int i = 0; i < 64; ++i) {
        if (reinterpret_cast<const u8*>(r1)[i] != static_cast<u8>(i)) preserved = false;
    }
    EXPECT(preserved, "realloc preserved first 64 bytes");

    // calloc returns zeroed memory.
    const u64 c = HleDispatch(calloc_id, 16, 8, 0, 0, 0, 0, 0x2004, 0);
    EXPECT(c != 0, "calloc(16, 8)");
    bool zeroed = true;
    for (int i = 0; i < 128; ++i) {
        if (reinterpret_cast<const u8*>(c)[i] != 0) zeroed = false;
    }
    EXPECT(zeroed, "calloc block is zero-filled");

    // memalign honors large alignments.
    const u64 ma_id = SymbolId("libc", "memalign");
    const u64 m4096 = HleDispatch(ma_id, 4096, 100, 0, 0, 0, 0, 0x2005, 0);
    EXPECT(m4096 != 0, "memalign(4096, 100)");
    EXPECT_EQ(m4096 & 4095, (u64)0, "memalign result is 4096-aligned");

    // aligned_alloc basics.
    const u64 aa_id = SymbolId("libc", "aligned_alloc");
    const u64 a256 = HleDispatch(aa_id, 256, 512, 0, 0, 0, 0, 0x2006, 0);
    EXPECT(a256 != 0 && (a256 & 255) == 0, "aligned_alloc(256, 512) aligned");

    // posix_memalign: success path + EINVAL on bad alignment.
    guest_addr_t scratch = 0;
    EXPECT(Memory::Map(0, 0x1000, Memory::PROT_READ | Memory::PROT_WRITE, &scratch) == Memory::Status::Ok,
           "map scratch page for posix_memalign");
    const u64 pm_id = SymbolId("libc", "posix_memalign");
    EXPECT_EQ(HleDispatch(pm_id, scratch, 2048, 100, 0, 0, 0, 0x2007, 0), (u64)0,
              "posix_memalign(2048) -> 0");
    const u64 pm_ptr = Memory::Read<u64>(scratch);
    EXPECT(pm_ptr != 0 && (pm_ptr & 2047) == 0, "posix_memalign wrote aligned pointer");
    EXPECT_EQ(HleDispatch(pm_id, scratch, 3, 100, 0, 0, 0, 0x2008, 0), (u64)22,
              "posix_memalign(align=3) -> EINVAL");

    // free + reuse: a same-size malloc after free must succeed (and typically
    // recycles the freed block).
    HleDispatch(free_id, p, 0, 0, 0, 0, 0, 0x2009, 0);
    const u64 p3 = HleDispatch(malloc_id, 100, 0, 0, 0, 0, 0, 0x200A, 0);
    EXPECT(p3 != 0, "malloc after free succeeds");
    // free(nullptr) and double free must not crash the process.
    HleDispatch(free_id, 0, 0, 0, 0, 0, 0, 0x200B, 0);
    HleDispatch(free_id, p, 0, 0, 0, 0, 0, 0x200C, 0);

    Memory::Unmap(scratch, 0x1000);
}

// ---------------------------------------------------------------------------
// libSceSystemService — canned offline parameters.
// ---------------------------------------------------------------------------
void TestSystemService() {
    std::fprintf(stdout, "[TEST] libSceSystemService canned parameters\n");

    guest_addr_t buf = 0;
    EXPECT(Memory::Map(0, 0x1000, Memory::PROT_READ | Memory::PROT_WRITE, &buf) == Memory::Status::Ok,
           "map scratch page for system service");

    const u64 param_id = SymbolId("libSceSystemService", "sceSystemServiceParamGetInt");
    EXPECT(param_id != 0, "ParamGetInt resolves");

    EXPECT_EQ(HleDispatch(param_id, 1, buf, 0, 0, 0, 0, 0x3000, 0), (u64)0, "ParamGetInt(1) -> 0");
    EXPECT_EQ(Memory::Read<s32>(buf), (s32)1, "ParamGetInt(1) value 1");
    EXPECT_EQ(HleDispatch(param_id, 4, buf, 0, 0, 0, 0, 0x3001, 0), (u64)0, "ParamGetInt(4) -> 0");
    EXPECT_EQ(Memory::Read<s32>(buf), (s32)180, "ParamGetInt(4) value 180");
    EXPECT_EQ(HleDispatch(param_id, 999, buf, 0, 0, 0, 0, 0x3002, 0), (u64)0, "ParamGetInt(999) -> 0");
    EXPECT_EQ(Memory::Read<s32>(buf), (s32)0, "ParamGetInt(999) value 0");
    EXPECT_EQ(HleDispatch(param_id, 1, 0, 0, 0, 0, 0, 0x3003, 0), (u64)0x80A10003,
              "ParamGetInt(null) -> 0x80A10003");

    // GetStatus: 0x0C bytes, byte@6 = 1, rest zero.
    std::memset(reinterpret_cast<void*>(buf), 0xAA, 0x20);
    const u64 status_id = SymbolId("libSceSystemService", "sceSystemServiceGetStatus");
    EXPECT_EQ(HleDispatch(status_id, buf, 0, 0, 0, 0, 0, 0x3004, 0), (u64)0, "GetStatus -> 0");
    const u8* st = reinterpret_cast<const u8*>(buf);
    bool status_ok = st[6] == 1;
    for (int i = 0; i < 0x0C; ++i) {
        if (i != 6 && st[i] != 0) status_ok = false;
    }
    EXPECT(status_ok, "GetStatus struct: byte@6=1, rest zero");

    // GetStatus via the libkernel NID alias (real implementation, not the stub).
    EXPECT_EQ(HleDispatch(SymbolId("libkernel", "rPo6tV8D9bM#Q#R"), buf, 0, 0, 0, 0, 0, 0x3005, 0),
              (u64)0, "GetStatus via libkernel NID -> 0");

    // DisplaySafeAreaInfo: float 1.0 followed by 128 zero bytes.
    std::memset(reinterpret_cast<void*>(buf), 0xAA, 0x100);
    const u64 safe_id = SymbolId("libSceSystemService", "sceSystemServiceGetDisplaySafeAreaInfo");
    EXPECT_EQ(HleDispatch(safe_id, buf, 0, 0, 0, 0, 0, 0x3006, 0), (u64)0, "SafeAreaInfo -> 0");
    float ratio = 0.0f;
    std::memcpy(&ratio, reinterpret_cast<const void*>(buf), sizeof(float));
    EXPECT(ratio == 1.0f, "SafeAreaInfo ratio 1.0");
    bool tail_zero = true;
    for (int i = 4; i < 132; ++i) {
        if (st[i] != 0) tail_zero = false;
    }
    EXPECT(tail_zero, "SafeAreaInfo 128-byte tail zeroed");

    EXPECT_EQ(HleDispatch(SymbolId("libSceSystemService", "sceSystemServiceHideSplashScreen"),
                          0, 0, 0, 0, 0, 0, 0x3007, 0),
              (u64)0, "HideSplashScreen -> 0");

    Memory::Unmap(buf, 0x1000);
}

// ---------------------------------------------------------------------------
// NP family — offline-but-signed-in.
// ---------------------------------------------------------------------------
void TestNp() {
    std::fprintf(stdout, "[TEST] NP offline-signed-in set\n");

    guest_addr_t buf = 0;
    EXPECT(Memory::Map(0, 0x1000, Memory::PROT_READ | Memory::PROT_WRITE, &buf) == Memory::Status::Ok,
           "map scratch page for np");

    // sceNpGetState writes 1 (signed in).
    Memory::Write<u32>(buf, 0xFFFFFFFF);
    const u64 state_id = SymbolId("libSceNpManager", "sceNpGetState");
    EXPECT(state_id != 0, "sceNpGetState resolves");
    EXPECT_EQ(HleDispatch(state_id, 1, buf, 0, 0, 0, 0, 0x4000, 0), (u64)0, "GetState -> 0");
    EXPECT_EQ(Memory::Read<u32>(buf), (u32)1, "GetState state == 1 (signed in)");

    // sceNpGetOnlineId -> 20-byte "Player" struct.
    std::memset(reinterpret_cast<void*>(buf), 0, 0x40);
    const u64 oid_id = SymbolId("libSceNpManager", "sceNpGetOnlineId");
    EXPECT_EQ(HleDispatch(oid_id, 1, buf, 0, 0, 0, 0, 0x4001, 0), (u64)0, "GetOnlineId -> 0");
    EXPECT(std::memcmp(reinterpret_cast<const void*>(buf), "Player", 6) == 0,
           "GetOnlineId name is \"Player\"");
    EXPECT_EQ(HleDispatch(oid_id, 1, 0, 0, 0, 0, 0, 0x4002, 0), (u64)0x80020016,
              "GetOnlineId(null) -> EINVAL");

    // Trophy2: context/handle creation hands out incrementing ids.
    const u64 ctx_id = SymbolId("libSceNpTrophy2", "sceNpTrophy2CreateContext");
    const u64 hdl_id = SymbolId("libSceNpTrophy2", "sceNpTrophy2CreateHandle");
    EXPECT(ctx_id && hdl_id, "trophy2 symbols resolve");
    Memory::Write<u32>(buf, 0);
    EXPECT_EQ(HleDispatch(ctx_id, buf, 0, 0, 0, 0, 0, 0x4003, 0), (u64)0, "CreateContext -> 0");
    const u32 first_ctx = Memory::Read<u32>(buf);
    EXPECT(first_ctx != 0, "CreateContext id non-zero");
    EXPECT_EQ(HleDispatch(ctx_id, buf, 0, 0, 0, 0, 0, 0x4004, 0), (u64)0, "CreateContext #2 -> 0");
    EXPECT(Memory::Read<u32>(buf) > first_ctx, "CreateContext ids increment");
    EXPECT_EQ(HleDispatch(hdl_id, buf, 0, 0, 0, 0, 0, 0x4005, 0), (u64)0, "CreateHandle -> 0");
    EXPECT(Memory::Read<u32>(buf) != 0, "CreateHandle id non-zero");

    // RegisterContext via libkernel NID alias is the real (success) impl.
    EXPECT_EQ(HleDispatch(SymbolId("libkernel", "Bagshr7OQ6Q#F#G"), buf, 0, 0, 0, 0, 0, 0x4006, 0),
              (u64)0, "CreateContext via libkernel NID -> 0");

    // GameIntent + NpCommon basics.
    EXPECT_EQ(HleDispatch(SymbolId("libSceNpGameIntent", "sceNpGameIntentInitialize"),
                          0, 0, 0, 0, 0, 0, 0x4007, 0),
              (u64)0, "NpGameIntentInitialize -> 0");
    EXPECT_EQ(HleDispatch(SymbolId("libSceNpCommon", "sceNpCmpNpId"), buf, buf, 0, 0, 0, 0, 0x4008, 0),
              (u64)0, "NpCmpNpId -> 0");

    Memory::Unmap(buf, 0x1000);
}

// ---------------------------------------------------------------------------
// libSceVideoOut — port model, buffer slots, flip counters, equeue events.
// ---------------------------------------------------------------------------
void TestVideoOut() {
    std::fprintf(stdout, "[TEST] libSceVideoOut port/flip model\n");

    guest_addr_t buf = 0;
    EXPECT(Memory::Map(0, 0x1000, Memory::PROT_READ | Memory::PROT_WRITE, &buf) == Memory::Status::Ok,
           "map scratch page for videoout");

    const u64 open_id  = SymbolId("libSceVideoOut", "sceVideoOutOpen");
    const u64 regb_id  = SymbolId("libSceVideoOut", "sceVideoOutRegisterBuffers");
    const u64 flip_id  = SymbolId("libSceVideoOut", "sceVideoOutSubmitFlip");
    const u64 stat_id  = SymbolId("libSceVideoOut", "sceVideoOutGetFlipStatus");
    const u64 addf_id  = SymbolId("libSceVideoOut", "sceVideoOutAddFlipEvent");
    const u64 addv_id  = SymbolId("libSceVideoOut", "sceVideoOutAddVblankEvent");
    EXPECT(open_id && regb_id && flip_id && stat_id && addf_id && addv_id,
           "videoout symbols resolve");

    const u64 handle = HleDispatch(open_id, 1, 0, 0, 0, 0, 0, 0x5000, 0);
    EXPECT(handle != 0, "sceVideoOutOpen returns a handle");

    // Register two buffers; attr block carries width/height at 0x10/0x14.
    u8* host = reinterpret_cast<u8*>(buf);
    std::memset(host, 0, 0x1000);
    Memory::Write<u64>(buf + 0x000, 0xAAAA0000); // buffer 0 address (bogus but registered)
    Memory::Write<u64>(buf + 0x008, 0xBBBB0000); // buffer 1 address
    Memory::Write<u32>(buf + 0x200 + 0x10, 640); // attr width
    Memory::Write<u32>(buf + 0x200 + 0x14, 480); // attr height
    EXPECT_EQ(HleDispatch(regb_id, handle, 0, buf, 2, buf + 0x200, 0, 0x5001, 0), (u64)0,
              "RegisterBuffers(2) -> 0");

    // Submit a flip on buffer 0; GetFlipStatus must reflect it.
    EXPECT_EQ(HleDispatch(flip_id, handle, 0, 0, 111, 0, 0, 0x5002, 0), (u64)0, "SubmitFlip(0) -> 0");
    std::memset(host, 0xAA, 0x40);
    EXPECT_EQ(HleDispatch(stat_id, handle, buf, 0, 0, 0, 0, 0x5003, 0), (u64)0, "GetFlipStatus -> 0");
    EXPECT_EQ(Memory::Read<u64>(buf + 0x00), (u64)1, "flip count == 1");
    EXPECT_EQ(Memory::Read<u64>(buf + 0x18), (u64)111, "flipArg == 111");
    EXPECT_EQ(Memory::Read<u64>(buf + 0x20), (u64)0, "currentBuffer == 0");
    EXPECT(Memory::Read<u64>(buf + 0x10) != 0, "tsc field populated");

    // Unregistered buffer index is rejected.
    EXPECT_EQ(HleDispatch(flip_id, handle, 5, 0, 0, 0, 0, 0x5004, 0), (u64)0x8029000A,
              "SubmitFlip(unregistered) -> 0x8029000A");
    // Bad handle is rejected.
    EXPECT_EQ(HleDispatch(flip_id, 0xDEAD, 0, 0, 0, 0, 0, 0x5005, 0), (u64)0x8029000B,
              "SubmitFlip(bad handle) -> 0x8029000B");

    // Wire an equeue and register for flip events.
    const u64 eq_out = buf + 0x100;
    HLE::GuestArgs eq_args{};
    eq_args.arg1 = eq_out;
    eq_args.arg2 = 0;
    EXPECT_EQ(HLE::SceKernelCreateEqueue(eq_args), (u64)0, "CreateEqueue -> 0");
    const u32 equeue = Memory::Read<u32>(eq_out);
    EXPECT(equeue != 0, "equeue handle returned");

    EXPECT_EQ(HleDispatch(addf_id, equeue, handle, 0xABCD, 0, 0, 0, 0x5006, 0), (u64)0,
              "AddFlipEvent -> 0");
    // Bad equeue is rejected.
    EXPECT_EQ(HleDispatch(addf_id, 0x7777, handle, 0, 0, 0, 0, 0x5007, 0), (u64)0x8029000C,
              "AddFlipEvent(bad equeue) -> 0x8029000C");

    // Flip with flipArg=7 fires a FLIP kevent: ident 6, filter -13, hint in data.
    EXPECT_EQ(HleDispatch(flip_id, handle, 1, 0, 7, 0, 0, 0x5008, 0), (u64)0, "SubmitFlip(1) -> 0");

    // Poll the queue (timo = 0 usec at timo_ptr => poll mode).
    const u64 ev_buf  = buf + 0x300;
    const u64 out_cnt = buf + 0x3F0;
    const u64 timo    = buf + 0x3F8;
    Memory::Write<u32>(timo, 0);
    HLE::GuestArgs wait_args{};
    wait_args.arg1 = equeue;
    wait_args.arg2 = ev_buf;
    wait_args.arg3 = 1;
    wait_args.arg4 = out_cnt;
    wait_args.arg5 = timo;
    EXPECT_EQ(HLE::SceKernelWaitEqueue(wait_args), (u64)0, "WaitEqueue(poll) -> 0");
    EXPECT_EQ(Memory::Read<s32>(out_cnt), (s32)1, "one flip event delivered");
    EXPECT_EQ(Memory::Read<u64>(ev_buf + 0x00), (u64)6, "flip event ident == 6");
    EXPECT_EQ(Memory::Read<s16>(ev_buf + 0x08), (s16)-13, "flip event filter == -13");
    EXPECT_EQ(Memory::Read<u64>(ev_buf + 0x18), (u64)0xABCD, "flip event udata");
    const u64 ev_data = Memory::Read<u64>(ev_buf + 0x10);
    EXPECT_EQ(ev_data & 0xFFFF, (u64)6, "flip event data low bits == ident");
    EXPECT_EQ((ev_data >> 16) & 0xFFFFFFFFFFFFULL, (u64)7, "flip event data carries flipArg");

    // Flip count is now 2.
    EXPECT_EQ(HleDispatch(stat_id, handle, buf, 0, 0, 0, 0, 0x5009, 0), (u64)0, "GetFlipStatus #2 -> 0");
    EXPECT_EQ(Memory::Read<u64>(buf + 0x00), (u64)2, "flip count == 2");
    EXPECT_EQ(Memory::Read<u64>(buf + 0x20), (u64)1, "currentBuffer == 1");

    // AddVblankEvent signals one edge immediately.
    EXPECT_EQ(HleDispatch(addv_id, equeue, handle, 0x1234, 0, 0, 0, 0x500A, 0), (u64)0,
              "AddVblankEvent -> 0");
    Memory::Write<u32>(timo, 0);
    EXPECT_EQ(HLE::SceKernelWaitEqueue(wait_args), (u64)0, "WaitEqueue(vblank poll) -> 0");
    EXPECT_EQ(Memory::Read<s32>(out_cnt), (s32)1, "one vblank event delivered");
    EXPECT_EQ(Memory::Read<u64>(ev_buf + 0x00), (u64)5, "vblank event ident == 5");
    EXPECT_EQ(Memory::Read<u64>(ev_buf + 0x18), (u64)0x1234, "vblank event udata");

    // GetEventId / GetEventData on the delivered kevent layout.
    std::memcpy(host, reinterpret_cast<const void*>(ev_buf), 0x20);
    Memory::Write<u64>(buf + 0x00, 5);   // ident
    Memory::Write<s16>(buf + 0x08, -13); // filter
    Memory::Write<u64>(buf + 0x10, 0xDEAD);
    const u64 geid_id = SymbolId("libSceVideoOut", "sceVideoOutGetEventId");
    const u64 ged_id  = SymbolId("libSceVideoOut", "sceVideoOutGetEventData");
    EXPECT_EQ(HleDispatch(geid_id, buf, 0, 0, 0, 0, 0, 0x500B, 0), (u64)0, "GetEventId(valid) -> 0");
    Memory::Write<s16>(buf + 0x08, -5);
    EXPECT_EQ(HleDispatch(geid_id, buf, 0, 0, 0, 0, 0, 0x500C, 0), (u64)0x8029000D,
              "GetEventId(wrong filter) -> 0x8029000D");
    Memory::Write<u64>(buf + 0x00, 5);
    EXPECT_EQ(HleDispatch(ged_id, buf, buf + 0x20, 0, 0, 0, 0, 0x500D, 0), (u64)0,
              "GetEventData -> 0");
    EXPECT_EQ(Memory::Read<u64>(buf + 0x20), (u64)0xDEAD, "GetEventData value");

    Memory::Unmap(buf, 0x1000);
}

// ---------------------------------------------------------------------------
// NID-database gap-filler stubs.
// ---------------------------------------------------------------------------
void TestNidDbStubs() {
    std::fprintf(stdout, "[TEST] NID-database stub registration\n");

    const std::string db_path = "pcsx5_test_phase3_nid_db.txt";
    {
        std::ofstream db(db_path, std::ios::binary | std::ios::trunc);
        // A brand-new entry that no module implements...
        db << "AbCdEfGhIjK\ttestmod\ttestDbStubFunc\n";
        // ...and an entry whose implementation already exists (must NOT be
        // replaced by a stub).
        db << "pZ9WXcClPO8\tlibkernel\tsceKernelMapDirectMemory\n";
    }
    EXPECT(Common::LoadNidDatabase(db_path), "temp NID db loads");
    std::remove(db_path.c_str());

    HLE::RegisterNidDbStubs();

    // The new entry resolves under its real name and returns 0 when called.
    const guest_addr_t thunk = HLE::ResolveAny("testDbStubFunc");
    EXPECT(thunk != 0, "DB stub resolves by real name");
    EXPECT(HLE::GetUnresolvedImportCount() == 0, "DB stub is not an auto-stub");
    EXPECT_EQ(HleDispatch(ReadSymbolIdFromThunk(thunk), 0, 0, 0, 0, 0, 0, 0x6000, 0), (u64)0,
              "DB stub returns 0");

    // The pre-existing real implementation survived RegisterNidDbStubs: the
    // real MapDirectMemory handler rejects a null addr_ptr with 0x800D0004,
    // whereas a stub would have returned 0.
    const u64 mdm_id = SymbolId("libkernel", "sceKernelMapDirectMemory");
    EXPECT(mdm_id != 0, "sceKernelMapDirectMemory resolves");
    EXPECT_EQ(HleDispatch(mdm_id, 0, 0x1000, 3, 0, 0, 0, 0x6001, 0), (u64)0x800D0004,
              "real MapDirectMemory impl not overwritten by DB stub");
}

} // namespace

// ---------------------------------------------------------------------------
// Direct-memory mapping semantics (LOST EPIC boot blocker).
//
//   * sceKernelAllocateMainDirectMemory returns a physical-pool OFFSET (like
//     sceKernelAllocateDirectMemory), not a host VA.
//   * sceKernelMapDirectMemory with no hint and no physOff (a pure VA
//     reservation / anonymous mapping) returns a DISTINCT address per call;
//     an earlier revision aliased every such mapping at the phys pool base,
//     which corrupted the guest heap.
// ---------------------------------------------------------------------------
void TestDirectMemoryMapping() {
    std::fprintf(stdout, "[TEST] direct memory mapping semantics\n");

    const u64 alloc_id = SymbolId("libkernel", "sceKernelAllocateMainDirectMemory");
    const u64 map_id   = SymbolId("libkernel", "sceKernelMapDirectMemory");
    const u64 mprot_id = SymbolId("libkernel", "sceKernelMprotect");
    EXPECT(alloc_id && map_id && mprot_id, "direct-memory symbols resolve");

    // Scratch space for out-params.
    guest_addr_t out = 0;
    EXPECT_EQ(Memory::Map(0, 0x1000, Memory::PROT_READ | Memory::PROT_WRITE, &out),
              Memory::Status::Ok, "scratch out-param page mapped");

    // AllocateMainDirectMemory -> sequential pool offsets, 0x10000-based.
    EXPECT_EQ(HleDispatch(alloc_id, 0x4000, 0, 12, out, 0, 0, 0x7000, 0), (u64)0,
              "AllocateMainDirectMemory #1 ok");
    const u64 off1 = Memory::Read<u64>(out);
    EXPECT(off1 >= 0x10000 && off1 < (2ULL << 30), "phys offset #1 is a pool offset, not a VA");
    EXPECT_EQ(HleDispatch(alloc_id, 0x100000, 0, 12, out, 0, 0, 0x7001, 0), (u64)0,
              "AllocateMainDirectMemory #2 ok");
    const u64 off2 = Memory::Read<u64>(out);
    EXPECT(off2 >= off1 + 0x4000, "phys offset #2 beyond #1 allocation");

    // Pure VA reservations (prot=0, physOff=0, hint=0): distinct addresses.
    Memory::Write<u64>(out, 0);
    EXPECT_EQ(HleDispatch(map_id, out, 0x40000, 0, 0, 0, 0, 0x7002, 0), (u64)0,
              "reservation #1 ok");
    const u64 va1 = Memory::Read<u64>(out);
    Memory::Write<u64>(out, 0);
    EXPECT_EQ(HleDispatch(map_id, out, 0x100000, 0, 0, 0, 0, 0x7003, 0), (u64)0,
              "reservation #2 ok");
    const u64 va2 = Memory::Read<u64>(out);
    EXPECT(va1 != 0 && va2 != 0, "reservations returned VAs");
    EXPECT(va1 != va2, "reservations are distinct (no pool-base aliasing)");

    // Reserved, not committed; mprotect commits a subrange in place.
    Memory::MemoryInfo qi{};
    EXPECT_EQ(Memory::Query(va1, &qi), Memory::Status::Ok, "reservation #1 queryable");
    EXPECT(qi.is_reserved && !qi.is_committed, "reservation #1 starts uncommitted");
    EXPECT_EQ(HleDispatch(mprot_id, va1, 0x4000, 0x33, 0, 0, 0, 0x7004, 0), (u64)0,
              "mprotect commits subrange");
    Memory::Write<u64>(va1, 0x1122334455667788ULL);
    EXPECT_EQ(Memory::Read<u64>(va1), 0x1122334455667788ULL, "committed subrange is writable");

    // Phys-backed mapping (hint=0, physOff!=0): committed and writable.
    Memory::Write<u64>(out, 0);
    EXPECT_EQ(HleDispatch(map_id, out, 0x4000, 0x33, 0, off1, 0, 0x7005, 0), (u64)0,
              "phys-backed mapping ok");
    const u64 va3 = Memory::Read<u64>(out);
    EXPECT(va3 != 0 && va3 != va1 && va3 != va2, "phys-backed mapping has own VA");
    Memory::Write<u64>(va3, 0xA5A5A5A5A5A5A5A5ULL);
    EXPECT_EQ(Memory::Read<u64>(va3), 0xA5A5A5A5A5A5A5A5ULL, "phys-backed mapping writable");

    Memory::Unmap(out, 0x1000);
}

// ---------------------------------------------------------------------------
// libkernel clock_gettime (lLMT9vJAck0) — realtime + monotonic guest clocks.
// ---------------------------------------------------------------------------
void TestClockGettime() {
    std::fprintf(stdout, "[TEST] libkernel clock_gettime\n");

    const u64 gt_id = SymbolId("libkernel", "clock_gettime");
    const u64 gt_nid = SymbolIdAny("lLMT9vJAck0");
    EXPECT(gt_id != 0, "clock_gettime resolves");
    EXPECT(gt_nid != 0, "clock_gettime NID lLMT9vJAck0 resolves");

    guest_addr_t ts = 0;
    EXPECT_EQ(Memory::Map(0, 0x1000, Memory::PROT_READ | Memory::PROT_WRITE, &ts),
              Memory::Status::Ok, "timespec page mapped");

    // CLOCK_REALTIME (0): plausible Unix time, sane nanoseconds.
    EXPECT_EQ(HleDispatch(gt_id, 0, ts, 0, 0, 0, 0, 0x7100, 0), (u64)0,
              "clock_gettime(CLOCK_REALTIME) -> 0");
    const u64 rt_sec = Memory::Read<u64>(ts);
    const u64 rt_nsec = Memory::Read<u64>(ts + 8);
    EXPECT(rt_sec > 1700000000ull, "realtime tv_sec is a plausible Unix time");
    EXPECT(rt_nsec < 1000000000ull, "realtime tv_nsec < 1e9");

    // Monotonic (any non-zero id): shared QPC clock (raw counter / freq, as in
    // sys_clock_gettime), non-decreasing.
    EXPECT_EQ(HleDispatch(gt_id, 4, ts, 0, 0, 0, 0, 0x7101, 0), (u64)0,
              "clock_gettime(CLOCK_MONOTONIC) -> 0");
    const u64 m_sec0 = Memory::Read<u64>(ts);
    const u64 m_nsec0 = Memory::Read<u64>(ts + 8);
    EXPECT(m_nsec0 < 1000000000ull, "monotonic tv_nsec < 1e9");
    EXPECT_EQ(HleDispatch(gt_nid, 4, ts, 0, 0, 0, 0, 0x7102, 0), (u64)0,
              "clock_gettime via NID -> 0");
    const u64 m_sec1 = Memory::Read<u64>(ts);
    const u64 m_nsec1 = Memory::Read<u64>(ts + 8);
    EXPECT(m_sec1 > m_sec0 || (m_sec1 == m_sec0 && m_nsec1 >= m_nsec0),
           "monotonic clock is non-decreasing");

    // Null timespec -> -1.
    EXPECT_EQ(HleDispatch(gt_id, 0, 0, 0, 0, 0, 0, 0x7103, 0), (u64)-1,
              "clock_gettime(null ts) -> -1");

    Memory::Unmap(ts, 0x1000);
}

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

    TestSysmodule();
    TestGuestHeap();
    TestSystemService();
    TestNp();
    TestVideoOut();
    TestNidDbStubs();
    TestDirectMemoryMapping();
    TestClockGettime();

    HLE::Shutdown();
    Memory::Shutdown();

    if (g_failures == 0) {
        std::fprintf(stdout, "HLE Phase-3 tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "HLE Phase-3 tests: %d failure(s).\n", g_failures);
    return 1;
}
