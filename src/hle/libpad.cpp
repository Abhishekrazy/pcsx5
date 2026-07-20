#include "hle.h"
#include "../gpu/gpu.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include <windows.h>
#include <cstring>
#include <chrono>

namespace HLE {

    // ScePadData — retail 0x78-byte layout (matches the reference emulator's
    // PadExports.cs WriteNeutralPadData offsets):
    //   0x00 u32  buttons (SCE_PAD_BUTTON bitmask; 0x100000 = touchpad click)
    //   0x04 u8   lx, 0x05 ly, 0x06 rx, 0x07 ry (0..255, 128 centered)
    //   0x08 u8   l2, 0x09 r2 (0..255)
    //   0x18 f32  1.0f (touch-pad orientation scale)
    //   0x20      ScePadTouchData { u8 touchNum; u8 reserved[3]; touch[2] }
    //             each touch: { u16 x; u16 y; u8 id; u8 reserved; }
    //   0x4C u8   connected (touch info)
    //   0x50 u64  timestamp (microseconds)
    //   0x68 u8   connected count
    struct ScePadTouch {
        u16 x;        // 0..1919
        u16 y;        // 0..942
        u8 id;
        u8 reserved;
    };
    struct ScePadData {
        u32 buttons;          // 0x00
        u8 lx, ly, rx, ry;    // 0x04
        u8 l2, r2;            // 0x08
        u8 reserved0[0x0E];   // 0x0A
        float touch_scale;    // 0x18 (1.0f)
        u8 reserved1[0x04];   // 0x1C
        u8 touch_num;         // 0x20
        u8 touch_reserved[3]; // 0x21
        ScePadTouch touch[2]; // 0x24
        u8 reserved2[0x1C];   // 0x30
        u8 touch_connected;   // 0x4C
        u8 reserved3[0x03];   // 0x4D
        u64 timestamp;        // 0x50 (microseconds)
        u8 reserved4[0x10];   // 0x58
        u8 connected_count;   // 0x68
        u8 reserved5[0x0F];   // 0x69
    };
    static_assert(sizeof(ScePadData) == 0x78, "ScePadData must be 0x78 bytes");

    // ORBIS_PAD error codes (matches SharpEmu PadExports.cs).
    static constexpr u32 SCE_PAD_ERROR_INVALID_HANDLE      = 0x80920003;
    static constexpr u32 SCE_PAD_ERROR_NOT_INITIALIZED     = 0x80920005;
    static constexpr u32 SCE_PAD_ERROR_DEVICE_NOT_CONNECTED = 0x80920007;
    static constexpr u32 SCE_PAD_ERROR_DEVICE_NO_HANDLE    = 0x80920008;
    static constexpr u32 ORBIS_GEN2_ERROR_INVALID_ARGUMENT = 0x80020003;

    // Keep the pad session on the same retail user id returned by
    // libSceUserService.  A mismatched emulator-local id makes games pass a
    // valid 0x10000000 user to scePadOpen and receive DEVICE_NOT_CONNECTED,
    // leaving every later keyboard/gamepad read on an invalid handle.
    static constexpr u32 SCE_PAD_PRIMARY_USER_ID = 0x10000000;
    static constexpr u32 SCE_PAD_PRIMARY_HANDLE  = 1;

    static bool g_pad_initialized = false;

    // Real firmware hands out small non-negative handles; 0 is valid.  Some
    // titles read pad state with handle 0, and rejecting it leaves their
    // controller init path polling a never-valid state forever.
    static bool IsPrimaryPadHandle(u32 handle) {
        return handle == 0 || handle == SCE_PAD_PRIMARY_HANDLE;
    }

    static u64 NowMicroseconds() {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    static void FillPadData(ScePadData* out) {
        std::memset(out, 0, sizeof(*out));
        GPU::PadButtonState state = GPU::GetCurrentPadState();
        out->buttons = state.buttons;
        out->lx = state.left_analog_x;
        out->ly = state.left_analog_y;
        out->rx = state.right_analog_x;
        out->ry = state.right_analog_y;
        out->l2 = state.l2_trigger;
        out->r2 = state.r2_trigger;
        out->touch_scale = 1.0f;
        // Touch state: the click itself rides in the buttons bitmask (T key /
        // XInput BACK -> 0x100000); touchNum stays 0 (neutral, no fingers).
        // Real finger positions need the DualSense HID path (see the C#
        // launcher's WindowsDualSenseReader.cs) — not integrated here.
        out->touch_num = 0;
        out->touch_connected = 1;
        out->timestamp = NowMicroseconds();
        out->connected_count = 1;
    }

    void RegisterLibPad() {
        LOG_INFO(HLE, "Registering libScePad HLE symbols...");

        // Handler bodies are shared between the plain-name registrations and
        // the raw-NID registrations under "libkernel" below: the games we
        // boot resolve pad imports module-scoped against libkernel by NID
        // (e.g. libkernel::xk0AcarP3V4#L#M == scePadOpen), which the
        // cross-module friendly-name bridge in Resolve() cannot reach.

        auto PadInit = [](const GuestArgs& args) -> u64 {
            (void)args;
            LOG_INFO(HLE, "scePadInit() called");
            g_pad_initialized = true;
            return 0; // Success
        };
        RegisterSymbol("libScePad", "scePadInit", PadInit);
        RegisterSymbol("libkernel", "hv1luiJrqQM#L#M", PadInit);

        // scePadOpen rejects a non-null 4th arg and non-standard port types;
        // scePadOpenExt accepts a ScePadOpenExtParam* plus port types 1/2
        // (racing titles retry scePadOpenExt(type=2) forever if rejected).
        auto PadOpenCore = [](const GuestArgs& args, bool extended) -> u64 {
            u32 userId = static_cast<u32>(args.arg1);
            u32 type = static_cast<u32>(args.arg2);
            u32 index = static_cast<u32>(args.arg3);
            guest_addr_t opt = args.arg4;

            LOG_INFO(HLE, "scePadOpen%s(userId: 0x%X, type: %u, index: %u) called",
                     extended ? "Ext" : "", userId, type, index);

            if (!g_pad_initialized) {
                return SCE_PAD_ERROR_NOT_INITIALIZED;
            }
            if (userId == 0xFFFFFFFFu) { // userId == -1
                return SCE_PAD_ERROR_DEVICE_NO_HANDLE;
            }
            const bool type_accepted = extended ? (type <= 2) : (type == 0);
            if (userId != SCE_PAD_PRIMARY_USER_ID || !type_accepted || index != 0 ||
                (!extended && opt != 0)) {
                return SCE_PAD_ERROR_DEVICE_NOT_CONNECTED;
            }
            return SCE_PAD_PRIMARY_HANDLE;
        };
        auto PadOpen = [PadOpenCore](const GuestArgs& args) -> u64 {
            return PadOpenCore(args, false);
        };
        RegisterSymbol("libScePad", "scePadOpen", PadOpen);
        RegisterSymbol("libkernel", "xk0AcarP3V4#L#M", PadOpen);

        auto PadOpenExt = [PadOpenCore](const GuestArgs& args) -> u64 {
            return PadOpenCore(args, true);
        };
        RegisterSymbol("libScePad", "scePadOpenExt", PadOpenExt);
        RegisterSymbol("libkernel", "WFIiSfXGUq8#L#M", PadOpenExt);

        // scePadGetHandle(userId, type, index): returns the handle of the
        // already-open primary pad without opening a new one.  Some titles
        // call it every frame to poll input; same validation as scePadOpen.
        auto PadGetHandle = [](const GuestArgs& args) -> u64 {
            u32 userId = static_cast<u32>(args.arg1);
            u32 type = static_cast<u32>(args.arg2);
            u32 index = static_cast<u32>(args.arg3);
            if (!g_pad_initialized) {
                return SCE_PAD_ERROR_NOT_INITIALIZED;
            }
            if (userId != SCE_PAD_PRIMARY_USER_ID || type > 2 || index != 0) {
                return SCE_PAD_ERROR_DEVICE_NOT_CONNECTED;
            }
            return SCE_PAD_PRIMARY_HANDLE;
        };
        RegisterSymbol("libScePad", "scePadGetHandle", PadGetHandle);
        RegisterSymbol("libkernel", "u1GRHp+oWoY#L#M", PadGetHandle);

        auto PadClose = [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            LOG_INFO(HLE, "scePadClose(handle: 0x%X) called", handle);
            return IsPrimaryPadHandle(handle) ? 0 : SCE_PAD_ERROR_INVALID_HANDLE;
        };
        RegisterSymbol("libScePad", "scePadClose", PadClose);
        RegisterSymbol("libkernel", "6ncge5+l5Qs#L#M", PadClose);

        auto PadReadState = [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            guest_addr_t data_ptr = args.arg2;

            if (!IsPrimaryPadHandle(handle)) {
                return SCE_PAD_ERROR_INVALID_HANDLE;
            }
            if (!data_ptr) {
                return ORBIS_GEN2_ERROR_INVALID_ARGUMENT;
            }

            // Retrieve keyboard/gamepad-mapped controller state from GPU/GLFW
            ScePadData pad_data;
            FillPadData(&pad_data);
            Memory::WriteBuffer(data_ptr, &pad_data, sizeof(ScePadData));
            return 0; // Success
        };
        RegisterSymbol("libScePad", "scePadReadState", PadReadState);
        RegisterSymbol("libkernel", "YndgXqQVV7c#L#M", PadReadState);

        // scePadRead(handle, ScePadData* data, int count): buffered read —
        // fills up to `count` entries and returns the number actually read.
        // We snapshot the current host state into a small ring buffer, then
        // drain up to `count` entries, so callers always get at least one
        // fresh sample per call (the common case: 1 entry returned).
        auto PadRead = [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            guest_addr_t data_ptr = args.arg2;
            int count = static_cast<int>(args.arg3);

            if (!IsPrimaryPadHandle(handle)) {
                return SCE_PAD_ERROR_INVALID_HANDLE;
            }
            if (!data_ptr || count < 1 || count > 64) {
                return ORBIS_GEN2_ERROR_INVALID_ARGUMENT;
            }

            static ScePadData s_ring[64];
            static u32 s_ring_head = 0; // next write position
            static u32 s_ring_size = 0; // entries available

            FillPadData(&s_ring[s_ring_head]);
            s_ring_head = (s_ring_head + 1) % 64;
            if (s_ring_size < 64) s_ring_size++;

            const u32 to_read = static_cast<u32>(count) < s_ring_size
                ? static_cast<u32>(count) : s_ring_size;
            // Drain oldest-first.
            const u32 oldest = (s_ring_head + 64 - s_ring_size) % 64;
            for (u32 i = 0; i < to_read; ++i) {
                const ScePadData& entry = s_ring[(oldest + i) % 64];
                Memory::WriteBuffer(data_ptr + static_cast<u64>(i) * sizeof(ScePadData),
                                    &entry, sizeof(ScePadData));
            }
            s_ring_size -= to_read;
            return to_read;
        };
        RegisterSymbol("libScePad", "scePadRead", PadRead);
        RegisterSymbol("libkernel", "q1cHNfGycLI#L#M", PadRead);

        // scePadSetMotionSensorState(handle, enabled, ...) — no-op success.
        auto PadNoop0 = [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            return IsPrimaryPadHandle(handle) ? 0 : SCE_PAD_ERROR_INVALID_HANDLE;
        };
        RegisterSymbol("libkernel", "clVvL4ZDntw#L#M", PadNoop0);

        // scePadGetControllerInformation(handle, ScePadControllerInformation* out)
        // — report a connected DualSense-style controller (0x1C-byte struct,
        // matching the reference emulator's values).
        auto PadGetControllerInformation = [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            guest_addr_t out = args.arg2;
            if (!IsPrimaryPadHandle(handle)) {
                return SCE_PAD_ERROR_INVALID_HANDLE;
            }
            if (!out) {
                return ORBIS_GEN2_ERROR_INVALID_ARGUMENT;
            }
            u8 info[0x1C] = {};
            const float touch_density = 44.86f;
            std::memcpy(&info[0x00], &touch_density, sizeof(float));
            const u16 touch_res_x = 1920, touch_res_y = 943;
            std::memcpy(&info[0x04], &touch_res_x, sizeof(u16));
            std::memcpy(&info[0x06], &touch_res_y, sizeof(u16));
            info[0x08] = 30; // stick deadzone L
            info[0x09] = 30; // stick deadzone R
            info[0x0A] = 0;  // port type: standard
            info[0x0B] = 1;  // connected count
            info[0x0C] = 1;  // connected
            Memory::WriteBuffer(out, info, sizeof(info));
            return 0;
        };
        RegisterSymbol("libkernel", "gjP9-KQzoUk#L#M", PadGetControllerInformation);
    }
}
