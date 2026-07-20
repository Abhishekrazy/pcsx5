// Unit tests for the libSceAudioOut port state machine (src/hle/libaudioout.cpp)
// and the libScePad handle/read model (src/hle/libpad.cpp).
//
// Audio: Init/Open (valid + invalid params, all-8-ports-busy PORT_FULL),
// Output with NULL source (paced-silence path, backend=Off), SetVolume
// clamping, GetPortState contents, Close.  Tests set backend=0 (Off) via
// HLE::SetAudioOutConfig so no host audio device is required.
// Pad: scePadOpen validation, scePadGetHandle, scePadReadState filling the
// 0x78 ScePadData, scePadRead count validation and ring drain.
//
// Build target: hle_audio_pad_tests (see CMakeLists.txt).

#include "hle/hle.h"
#include "memory/memory.h"
#include "common/log.h"

#include <cstdio>
#include <cstring>
#include <chrono>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" u64 HleDispatch(u64, u64, u64, u64, u64, u64, u64, u64, u64);

namespace {

// Read the symbol_id encoded in a thunk (same pattern as hle_phase3_tests).
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

u64 SymbolId(const char* module, const char* name) {
    return ReadSymbolIdFromThunk(HLE::Resolve(module, name));
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

constexpr u64 kErrInvalidArg   = 0x8002000D; // SCE_KERNEL_ERROR_EINVAL
constexpr u64 kErrPortFull     = 0x802E0009; // SCE_AUDIO_OUT_ERROR_PORT_FULL

constexpr u64 SCE_PAD_ERROR_INVALID_HANDLE       = 0x80920003;
constexpr u64 SCE_PAD_ERROR_NOT_INITIALIZED      = 0x80920005;
constexpr u64 SCE_PAD_ERROR_DEVICE_NOT_CONNECTED = 0x80920007;
constexpr u64 SCE_PAD_ERROR_DEVICE_NO_HANDLE     = 0x80920008;
constexpr u64 ORBIS_GEN2_ERROR_INVALID_ARGUMENT  = 0x80020003;

constexpr u32 kPrimaryUser = 0x10000000;

// ---------------------------------------------------------------------------
// libSceAudioOut — port state machine.
// ---------------------------------------------------------------------------
void TestAudioOut() {
    std::fprintf(stdout, "[TEST] libSceAudioOut port state machine\n");

    // No host audio device in tests.
    HLE::SetAudioOutConfig(0, 1.0f);

    const u64 init_id       = SymbolId("libSceAudioOut", "sceAudioOutInit");
    const u64 open_id       = SymbolId("libSceAudioOut", "sceAudioOutOpen");
    const u64 close_id      = SymbolId("libSceAudioOut", "sceAudioOutClose");
    const u64 output_id     = SymbolId("libSceAudioOut", "sceAudioOutOutput");
    const u64 setvol_id     = SymbolId("libSceAudioOut", "sceAudioOutSetVolume");
    const u64 portstate_id  = SymbolId("libSceAudioOut", "sceAudioOutGetPortState");
    EXPECT(init_id && open_id && close_id && output_id && setvol_id && portstate_id,
           "audioout symbols resolve");

    // Init always succeeds.
    EXPECT_EQ(HleDispatch(init_id, 0, 0, 0, 0, 0, 0, 0x1000, 0), (u64)0,
              "sceAudioOutInit -> 0");

    // --- Invalid Open params -------------------------------------------------
    // buffer_length == 0.
    EXPECT_EQ(HleDispatch(open_id, kPrimaryUser, 0, 0, 0, 48000, 1, 0x1001, 0),
              kErrInvalidArg, "Open(len=0) -> EINVAL");
    // frequency == 0.
    EXPECT_EQ(HleDispatch(open_id, kPrimaryUser, 0, 0, 256, 0, 1, 0x1002, 0),
              kErrInvalidArg, "Open(freq=0) -> EINVAL");
    // unknown format (low byte 0x0F).
    EXPECT_EQ(HleDispatch(open_id, kPrimaryUser, 0, 0, 256, 48000, 0x0F, 0x1003, 0),
              kErrInvalidArg, "Open(bad fmt) -> EINVAL");

    // --- Valid Open + Output + SetVolume + GetPortState + Close --------------
    // format 1 = s16 stereo, 256 frames @ 48 kHz.
    const u64 h1 = HleDispatch(open_id, kPrimaryUser, 0, 0, 256, 48000, 1, 0x1004, 0);
    EXPECT(h1 >= 1 && h1 <= 8, "Open(valid) returns handle 1..8");

    // Output on a bad handle fails; NULL-source Output on the open port
    // succeeds (paced-silence path — returns 0 without a host device).
    EXPECT_EQ(HleDispatch(output_id, 0x7E, 0, 0, 0, 0, 0, 0x1005, 0), kErrInvalidArg,
              "Output(bad handle) -> EINVAL");
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(HleDispatch(output_id, h1, 0, 0, 0, 0, 0, 0x1006, 0), (u64)0,
              "Output(NULL src) -> 0 (paced silence)");
    EXPECT_EQ(HleDispatch(output_id, h1, 0, 0, 0, 0, 0, 0x1007, 0), (u64)0,
              "second Output(NULL src) -> 0");
    // Two consecutive silent outputs must pace to ~one buffer period total
    // (256/48000 s = 5.33 ms each; allow scheduler slack on the lower bound).
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT(elapsed_ms >= 5, "paced silence sleeps (>= ~one buffer period)");

    // Output with a real guest buffer also succeeds on the silent path.
    guest_addr_t buf = 0;
    EXPECT_EQ(Memory::Map(0, 0x1000, Memory::PROT_READ | Memory::PROT_WRITE, &buf),
              Memory::Status::Ok, "audio buffer page mapped");
    std::memset(reinterpret_cast<void*>(buf), 0, 0x1000);
    EXPECT_EQ(HleDispatch(output_id, h1, buf, 0, 0, 0, 0, 0x1008, 0), (u64)0,
              "Output(guest buf) -> 0");

    // SetVolume: bad handle fails; valid calls clamp to [0, 1] without error.
    guest_addr_t vols = 0;
    EXPECT_EQ(Memory::Map(0, 0x1000, Memory::PROT_READ | Memory::PROT_WRITE, &vols),
              Memory::Status::Ok, "volume page mapped");
    s32* vol_arr = reinterpret_cast<s32*>(vols);
    vol_arr[0] = 0x10000; vol_arr[1] = 0x10000; // 2x unity -> clamps to 1.0
    EXPECT_EQ(HleDispatch(setvol_id, 0x7E, 0x3, vols, 0, 0, 0, 0x1009, 0),
              kErrInvalidArg, "SetVolume(bad handle) -> EINVAL");
    EXPECT_EQ(HleDispatch(setvol_id, h1, 0x3, vols, 0, 0, 0, 0x100A, 0), (u64)0,
              "SetVolume(over-unity) -> 0 (clamped)");
    vol_arr[0] = -5; vol_arr[1] = -5;
    EXPECT_EQ(HleDispatch(setvol_id, h1, 0x3, vols, 0, 0, 0, 0x100B, 0), (u64)0,
              "SetVolume(negative) -> 0 (clamped)");
    vol_arr[0] = 32768 / 2; vol_arr[1] = 32768 / 2;
    EXPECT_EQ(HleDispatch(setvol_id, h1, 0x3, vols, 0, 0, 0, 0x100C, 0), (u64)0,
              "SetVolume(half) -> 0");
    // NULL volume pointer is tolerated (no-op success).
    EXPECT_EQ(HleDispatch(setvol_id, h1, 0x3, 0, 0, 0, 0, 0x100D, 0), (u64)0,
              "SetVolume(NULL ptr) -> 0");

    // GetPortState: connected output, 2 channels, volume 127.
    guest_addr_t st = 0;
    EXPECT_EQ(Memory::Map(0, 0x1000, Memory::PROT_READ | Memory::PROT_WRITE, &st),
              Memory::Status::Ok, "port-state page mapped");
    std::memset(reinterpret_cast<void*>(st), 0xCC, 16);
    EXPECT_EQ(HleDispatch(portstate_id, 0x7E, st, 0, 0, 0, 0, 0x100E, 0),
              kErrInvalidArg, "GetPortState(bad handle) -> EINVAL");
    EXPECT_EQ(HleDispatch(portstate_id, h1, 0, 0, 0, 0, 0, 0x100F, 0),
              kErrInvalidArg, "GetPortState(NULL out) -> EINVAL");
    EXPECT_EQ(HleDispatch(portstate_id, h1, st, 0, 0, 0, 0, 0x1010, 0), (u64)0,
              "GetPortState -> 0");
    const u8* state = reinterpret_cast<const u8*>(st);
    EXPECT_EQ(state[0], (u8)1, "port state: output connected");
    EXPECT_EQ(state[2], (u8)2, "port state: 2 channels");
    EXPECT_EQ(state[7], (u8)127, "port state: volume 127");

    // Close: bad handle fails, valid handle closes, double-close fails.
    EXPECT_EQ(HleDispatch(close_id, 0x7E, 0, 0, 0, 0, 0, 0x1011, 0), kErrInvalidArg,
              "Close(bad handle) -> EINVAL");
    EXPECT_EQ(HleDispatch(close_id, h1, 0, 0, 0, 0, 0, 0x1012, 0), (u64)0,
              "Close -> 0");
    EXPECT_EQ(HleDispatch(close_id, h1, 0, 0, 0, 0, 0, 0x1013, 0), kErrInvalidArg,
              "double Close -> EINVAL");
    // Output after close fails.
    EXPECT_EQ(HleDispatch(output_id, h1, 0, 0, 0, 0, 0, 0x1014, 0), kErrInvalidArg,
              "Output after Close -> EINVAL");

    // --- All 8 ports busy -> PORT_FULL ---------------------------------------
    u64 handles[8] = {};
    for (int i = 0; i < 8; ++i) {
        handles[i] = HleDispatch(open_id, kPrimaryUser, 0, 0, 256, 48000, 1,
                                 0x1100 + static_cast<u64>(i), 0);
        EXPECT(handles[i] >= 1 && handles[i] <= 8, "Open returns valid handle");
    }
    EXPECT_EQ(HleDispatch(open_id, kPrimaryUser, 0, 0, 256, 48000, 1, 0x1108, 0),
              kErrPortFull, "Open with 8 ports busy -> 0x802E0009 PORT_FULL");
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(HleDispatch(close_id, handles[i], 0, 0, 0, 0, 0,
                              0x1110 + static_cast<u64>(i), 0), (u64)0,
                  "Close busy-port handle -> 0");
    }
    // A slot frees up after Close.
    const u64 h_re = HleDispatch(open_id, kPrimaryUser, 0, 0, 256, 48000, 1, 0x1118, 0);
    EXPECT(h_re >= 1 && h_re <= 8, "Open succeeds after ports freed");
    EXPECT_EQ(HleDispatch(close_id, h_re, 0, 0, 0, 0, 0, 0x1119, 0), (u64)0,
              "Close reopened port -> 0");

    Memory::Unmap(buf, 0x1000);
    Memory::Unmap(vols, 0x1000);
    Memory::Unmap(st, 0x1000);
}

// ---------------------------------------------------------------------------
// libScePad — handle validation and state reads.
// ---------------------------------------------------------------------------
void TestPad() {
    std::fprintf(stdout, "[TEST] libScePad handle/read model\n");

    const u64 init_id       = SymbolId("libScePad", "scePadInit");
    const u64 open_id       = SymbolId("libScePad", "scePadOpen");
    const u64 openext_id    = SymbolId("libScePad", "scePadOpenExt");
    const u64 gethandle_id  = SymbolId("libScePad", "scePadGetHandle");
    const u64 close_id      = SymbolId("libScePad", "scePadClose");
    const u64 readstate_id  = SymbolId("libScePad", "scePadReadState");
    const u64 read_id       = SymbolId("libScePad", "scePadRead");
    EXPECT(init_id && open_id && openext_id && gethandle_id && close_id &&
           readstate_id && read_id, "pad symbols resolve");

    // --- Open before Init -> NOT_INITIALIZED ---------------------------------
    EXPECT_EQ(HleDispatch(open_id, kPrimaryUser, 0, 0, 0, 0, 0, 0x2000, 0),
              SCE_PAD_ERROR_NOT_INITIALIZED, "Open before Init -> NOT_INITIALIZED");
    EXPECT_EQ(HleDispatch(gethandle_id, kPrimaryUser, 0, 0, 0, 0, 0, 0x2001, 0),
              SCE_PAD_ERROR_NOT_INITIALIZED, "GetHandle before Init -> NOT_INITIALIZED");

    EXPECT_EQ(HleDispatch(init_id, 0, 0, 0, 0, 0, 0, 0x2002, 0), (u64)0,
              "scePadInit -> 0");

    // --- scePadOpen validation ------------------------------------------------
    // userId == -1 -> DEVICE_NO_HANDLE.
    EXPECT_EQ(HleDispatch(open_id, 0xFFFFFFFFu, 0, 0, 0, 0, 0, 0x2003, 0),
              SCE_PAD_ERROR_DEVICE_NO_HANDLE, "Open(user=-1) -> DEVICE_NO_HANDLE");
    // wrong user -> DEVICE_NOT_CONNECTED.
    EXPECT_EQ(HleDispatch(open_id, 0x42, 0, 0, 0, 0, 0, 0x2004, 0),
              SCE_PAD_ERROR_DEVICE_NOT_CONNECTED, "Open(bad user) -> NOT_CONNECTED");
    // bad type -> DEVICE_NOT_CONNECTED.
    EXPECT_EQ(HleDispatch(open_id, kPrimaryUser, 1, 0, 0, 0, 0, 0x2005, 0),
              SCE_PAD_ERROR_DEVICE_NOT_CONNECTED, "Open(type=1) -> NOT_CONNECTED");
    // bad index -> DEVICE_NOT_CONNECTED.
    EXPECT_EQ(HleDispatch(open_id, kPrimaryUser, 0, 1, 0, 0, 0, 0x2006, 0),
              SCE_PAD_ERROR_DEVICE_NOT_CONNECTED, "Open(index=1) -> NOT_CONNECTED");
    // non-null 4th arg (opt) -> DEVICE_NOT_CONNECTED.
    EXPECT_EQ(HleDispatch(open_id, kPrimaryUser, 0, 0, 0x1234, 0, 0, 0x2007, 0),
              SCE_PAD_ERROR_DEVICE_NOT_CONNECTED, "Open(opt!=0) -> NOT_CONNECTED");
    // valid open -> handle 1.
    EXPECT_EQ(HleDispatch(open_id, kPrimaryUser, 0, 0, 0, 0, 0, 0x2008, 0), (u64)1,
              "Open(valid) -> handle 1");
    // scePadOpenExt accepts types 0..2.
    EXPECT_EQ(HleDispatch(openext_id, kPrimaryUser, 2, 0, 0, 0, 0, 0x2009, 0), (u64)1,
              "OpenExt(type=2) -> handle 1");
    EXPECT_EQ(HleDispatch(openext_id, kPrimaryUser, 3, 0, 0, 0, 0, 0x200A, 0),
              SCE_PAD_ERROR_DEVICE_NOT_CONNECTED, "OpenExt(type=3) -> NOT_CONNECTED");

    // --- scePadGetHandle -------------------------------------------------------
    EXPECT_EQ(HleDispatch(gethandle_id, kPrimaryUser, 0, 0, 0, 0, 0, 0x200B, 0),
              (u64)1, "GetHandle(valid) -> 1");
    EXPECT_EQ(HleDispatch(gethandle_id, 0x42, 0, 0, 0, 0, 0, 0x200C, 0),
              SCE_PAD_ERROR_DEVICE_NOT_CONNECTED, "GetHandle(bad user) -> NOT_CONNECTED");
    EXPECT_EQ(HleDispatch(gethandle_id, kPrimaryUser, 3, 0, 0, 0, 0, 0x200D, 0),
              SCE_PAD_ERROR_DEVICE_NOT_CONNECTED, "GetHandle(type=3) -> NOT_CONNECTED");

    // --- scePadReadState --------------------------------------------------------
    guest_addr_t data = 0;
    EXPECT_EQ(Memory::Map(0, 0x1000, Memory::PROT_READ | Memory::PROT_WRITE, &data),
              Memory::Status::Ok, "pad data page mapped");

    EXPECT_EQ(HleDispatch(readstate_id, 0x77, data, 0, 0, 0, 0, 0x200E, 0),
              SCE_PAD_ERROR_INVALID_HANDLE, "ReadState(bad handle) -> INVALID_HANDLE");
    EXPECT_EQ(HleDispatch(readstate_id, 1, 0, 0, 0, 0, 0, 0x200F, 0),
              ORBIS_GEN2_ERROR_INVALID_ARGUMENT, "ReadState(NULL data) -> EINVAL");
    std::memset(reinterpret_cast<void*>(data), 0xCC, 0x78);
    EXPECT_EQ(HleDispatch(readstate_id, 1, data, 0, 0, 0, 0, 0x2010, 0), (u64)0,
              "ReadState -> 0");
    const u8* pd = reinterpret_cast<const u8*>(data);
    // connected count at 0x68 == 1; touch_connected at 0x4C == 1; touch
    // scale float at 0x18 == 1.0f; timestamp at 0x50 non-zero.
    EXPECT_EQ(pd[0x68], (u8)1, "ScePadData connected_count == 1");
    EXPECT_EQ(pd[0x4C], (u8)1, "ScePadData touch_connected == 1");
    float touch_scale = 0.0f;
    std::memcpy(&touch_scale, pd + 0x18, sizeof(float));
    EXPECT(touch_scale == 1.0f, "ScePadData touch_scale == 1.0f");
    u64 timestamp = 0;
    std::memcpy(&timestamp, pd + 0x50, sizeof(u64));
    EXPECT(timestamp != 0, "ScePadData timestamp non-zero");
    // Handle 0 is accepted as a primary-pad alias.
    EXPECT_EQ(HleDispatch(readstate_id, 0, data, 0, 0, 0, 0, 0x2011, 0), (u64)0,
              "ReadState(handle 0) -> 0");

    // --- scePadRead count validation + ring drain --------------------------------
    EXPECT_EQ(HleDispatch(read_id, 0x77, data, 1, 0, 0, 0, 0x2012, 0),
              SCE_PAD_ERROR_INVALID_HANDLE, "Read(bad handle) -> INVALID_HANDLE");
    EXPECT_EQ(HleDispatch(read_id, 1, 0, 1, 0, 0, 0, 0x2013, 0),
              ORBIS_GEN2_ERROR_INVALID_ARGUMENT, "Read(NULL data) -> EINVAL");
    EXPECT_EQ(HleDispatch(read_id, 1, data, 0, 0, 0, 0, 0x2014, 0),
              ORBIS_GEN2_ERROR_INVALID_ARGUMENT, "Read(count=0) -> EINVAL");
    EXPECT_EQ(HleDispatch(read_id, 1, data, static_cast<u64>(static_cast<s64>(-3)), 0, 0, 0, 0x2015, 0),
              ORBIS_GEN2_ERROR_INVALID_ARGUMENT, "Read(count<0) -> EINVAL");
    EXPECT_EQ(HleDispatch(read_id, 1, data, 65, 0, 0, 0, 0x2016, 0),
              ORBIS_GEN2_ERROR_INVALID_ARGUMENT, "Read(count=65) -> EINVAL");
    EXPECT_EQ(HleDispatch(read_id, 1, data, 64, 0, 0, 0, 0x2017, 0), (u64)1,
              "Read(count=64) -> 1 entry (fresh snapshot)");
    std::memset(reinterpret_cast<void*>(data), 0xCC, 0x78);
    EXPECT_EQ(HleDispatch(read_id, 1, data, 1, 0, 0, 0, 0x2018, 0), (u64)1,
              "Read(count=1) -> 1 entry");
    EXPECT_EQ(reinterpret_cast<const u8*>(data)[0x68], (u8)1,
              "Read entry has connected_count == 1");

    // --- scePadClose -------------------------------------------------------------
    EXPECT_EQ(HleDispatch(close_id, 1, 0, 0, 0, 0, 0, 0x2019, 0), (u64)0,
              "Close(handle 1) -> 0");
    EXPECT_EQ(HleDispatch(close_id, 0x77, 0, 0, 0, 0, 0, 0x201A, 0),
              SCE_PAD_ERROR_INVALID_HANDLE, "Close(bad handle) -> INVALID_HANDLE");

    Memory::Unmap(data, 0x1000);
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

    TestAudioOut();
    TestPad();

    HLE::Shutdown();
    Memory::Shutdown();

    if (g_failures == 0) {
        std::fprintf(stdout, "HLE audio/pad tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "HLE audio/pad tests: %d failure(s).\n", g_failures);
    return 1;
}
