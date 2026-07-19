#include "hle.h"
#include "../gpu/gpu.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include <windows.h>
#include <cstring>
#include <chrono>

namespace HLE {

    // ScePadData structure matching PS4/PS5 controller data structure
    struct ScePadData {
        u32 buttons;
        u8 rx;
        u8 ry;
        u8 lx;
        u8 ly;
        u8 l2;
        u8 r2;
        u8 padding[2];
        u64 timestamp;
        u64 count;
    };

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
            return 0; // Success
        };
        RegisterSymbol("libScePad", "scePadInit", PadInit);
        RegisterSymbol("libkernel", "hv1luiJrqQM#L#M", PadInit);

        auto PadOpen = [](const GuestArgs& args) -> u64 {
            u32 userId = static_cast<u32>(args.arg1);
            u32 type = static_cast<u32>(args.arg2);
            u32 index = static_cast<u32>(args.arg3);
            guest_addr_t opt = args.arg4;
            (void)opt;

            LOG_INFO(HLE, "scePadOpen(userId: %u, type: %u, index: %u) called", userId, type, index);

            // Return a mock pad handle
            static u32 mock_pad_handle = 0x3000;
            return mock_pad_handle++;
        };
        RegisterSymbol("libScePad", "scePadOpen", PadOpen);
        RegisterSymbol("libkernel", "xk0AcarP3V4#L#M", PadOpen);

        auto PadClose = [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            LOG_INFO(HLE, "scePadClose(handle: 0x%X) called", handle);
            return 0; // Success
        };
        RegisterSymbol("libScePad", "scePadClose", PadClose);
        RegisterSymbol("libkernel", "6ncge5+l5Qs#L#M", PadClose);

        auto PadReadState = [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            (void)handle;
            guest_addr_t data_ptr = args.arg2;

            if (!data_ptr) {
                return 0x80100001; // Invalid pointer error code
            }

            // Retrieve keyboard-mapped controller buttons state from GPU/GLFW
            GPU::PadButtonState state = GPU::GetCurrentPadState();

            // Set up pad data structure
            ScePadData pad_data;
            pad_data.buttons = state.buttons;
            pad_data.rx = state.right_analog_x;
            pad_data.ry = state.right_analog_y;
            pad_data.lx = state.left_analog_x;
            pad_data.ly = state.left_analog_y;
            pad_data.l2 = state.l2_trigger;
            pad_data.r2 = state.r2_trigger;
            pad_data.padding[0] = 0;
            pad_data.padding[1] = 0;

            static u64 s_count = 0;
            pad_data.count = s_count++;
            pad_data.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();

            // Write to guest memory
            Memory::WriteBuffer(data_ptr, &pad_data, sizeof(ScePadData));

            return 0; // Success
        };
        RegisterSymbol("libScePad", "scePadReadState", PadReadState);
        RegisterSymbol("libkernel", "YndgXqQVV7c#L#M", PadReadState);

        // scePadSetMotionSensorState(handle, enabled, ...) — no-op success.
        auto PadNoop0 = [](const GuestArgs& args) -> u64 {
            (void)args;
            return 0;
        };
        RegisterSymbol("libkernel", "clVvL4ZDntw#L#M", PadNoop0);

        // scePadGetControllerInformation(handle, ScePadControllerInformation* out)
        // — report a connected DualSense-style controller.
        auto PadGetControllerInformation = [](const GuestArgs& args) -> u64 {
            guest_addr_t out = args.arg2;
            if (!out) return 0x80100001;
            // Conservatively zero the struct (the game only checks fields it
            // knows); real firmware reports touchpad/motion/stick capability.
            u8 zeros[0x120] = {};
            Memory::WriteBuffer(out, zeros, sizeof(zeros));
            return 0;
        };
        RegisterSymbol("libkernel", "gjP9-KQzoUk#L#M", PadGetControllerInformation);
    }
}
