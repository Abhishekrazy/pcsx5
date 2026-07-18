#include "hle.h"
#include "../common/log.h"
#include "../memory/memory.h"

namespace HLE {

    // Mock pad handle counter
    static u32 g_pad_handle_counter = 0x1000;

    void RegisterLibScePad() {
        LOG_INFO(HLE, "Registering libScePad HLE symbols...");

        // scePadOpen
        // Signature: scePadOpen(port, slot, config*)
        RegisterSymbol("libScePad", "scePadOpen", [](const GuestArgs& args) -> u64 {
            u32 port = static_cast<u32>(args.arg1);
            u32 slot = static_cast<u32>(args.arg2);
            guest_addr_t config = args.arg3;
            (void)config;

            LOG_INFO(HLE, "scePadOpen(port: %u, slot: %u, config: 0x%llx) called", port, slot, config);

            // Return a mock pad handle
            u32 handle = g_pad_handle_counter++;
            return static_cast<u64>(handle);
        });

        // scePadClose
        // Signature: scePadClose(handle)
        RegisterSymbol("libScePad", "scePadClose", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            LOG_INFO(HLE, "scePadClose(handle: 0x%X) called", handle);
            return 0; // Success
        });

        // scePadRead
        // Signature: scePadRead(handle, port, data*, count)
        RegisterSymbol("libScePad", "scePadRead", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            u32 port = static_cast<u32>(args.arg2);
            guest_addr_t data_ptr = args.arg3;
            u32 count = static_cast<u32>(args.arg4);
            (void)port;

            LOG_DEBUG(HLE, "scePadRead(handle: 0x%X, port: %u, data: 0x%llx, count: %u) called", handle, port, data_ptr, count);

            // Return 0 (no data read) - pad not connected
            // In a real implementation, this would fill the ScePadData structure
            if (data_ptr && count > 0) {
                // Zero out the pad data structure (ScePadData is typically 64 bytes)
                for (u32 i = 0; i < count * 64; i += 8) {
                    Memory::Write<u64>(data_ptr + i, 0);
                }
            }

            return 0; // Number of entries read (0 = no controller connected)
        });

        // scePadGetData
        // Signature: scePadGetData(port, data*)
        RegisterSymbol("libScePad", "scePadGetData", [](const GuestArgs& args) -> u64 {
            u32 port = static_cast<u32>(args.arg1);
            guest_addr_t data_ptr = args.arg2;

            LOG_DEBUG(HLE, "scePadGetData(port: %u, data: 0x%llx) called", port, data_ptr);

            // Return 0 (no data/error) - pad not connected
            if (data_ptr) {
                // Zero out the pad data structure
                for (u32 i = 0; i < 64; i += 8) {
                    Memory::Write<u64>(data_ptr + i, 0);
                }
            }

            return 0; // SCE_OK or error code
        });

        // scePadSetVibration
        // Signature: scePadSetVibration(handle, large_motor, small_motor)
        RegisterSymbol("libScePad", "scePadSetVibration", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            u32 large_motor = static_cast<u32>(args.arg2);
            u32 small_motor = static_cast<u32>(args.arg3);

            LOG_DEBUG(HLE, "scePadSetVibration(handle: 0x%X, large: %u, small: %u) called", handle, large_motor, small_motor);

            // Mock implementation - just log and return success
            return 0; // SCE_OK
        });

        // scePadGetVibration
        // Signature: scePadGetVibration(handle, large_motor*, small_motor*)
        RegisterSymbol("libScePad", "scePadGetVibration", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            guest_addr_t large_ptr = args.arg2;
            guest_addr_t small_ptr = args.arg3;

            LOG_DEBUG(HLE, "scePadGetVibration(handle: 0x%X) called", handle);

            if (large_ptr) {
                Memory::Write<u32>(large_ptr, 0);
            }
            if (small_ptr) {
                Memory::Write<u32>(small_ptr, 0);
            }

            return 0; // SCE_OK
        });

        // scePadSetLightBar
        // Signature: scePadSetLightBar(handle, r, g, b)
        RegisterSymbol("libScePad", "scePadSetLightBar", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            u32 r = static_cast<u32>(args.arg2);
            u32 g = static_cast<u32>(args.arg3);
            u32 b = static_cast<u32>(args.arg4);

            LOG_DEBUG(HLE, "scePadSetLightBar(handle: 0x%X, r: %u, g: %u, b: %u) called", handle, r, g, b);

            return 0; // SCE_OK
        });

        // scePadGetLightBar
        // Signature: scePadGetLightBar(handle, r*, g*, b*)
        RegisterSymbol("libScePad", "scePadGetLightBar", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            guest_addr_t r_ptr = args.arg2;
            guest_addr_t g_ptr = args.arg3;
            guest_addr_t b_ptr = args.arg4;

            LOG_DEBUG(HLE, "scePadGetLightBar(handle: 0x%X) called", handle);

            if (r_ptr) Memory::Write<u32>(r_ptr, 0);
            if (g_ptr) Memory::Write<u32>(g_ptr, 0);
            if (b_ptr) Memory::Write<u32>(b_ptr, 0);

            return 0; // SCE_OK
        });

        // scePadSetTriggerEffect
        // Signature: scePadSetTriggerEffect(handle, left_trigger, right_trigger)
        RegisterSymbol("libScePad", "scePadSetTriggerEffect", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            guest_addr_t left_ptr = args.arg2;
            guest_addr_t right_ptr = args.arg3;
            (void)left_ptr;
            (void)right_ptr;

            LOG_DEBUG(HLE, "scePadSetTriggerEffect(handle: 0x%X) called", handle);

            return 0; // SCE_OK
        });

        // scePadGetTriggerEffect
        // Signature: scePadGetTriggerEffect(handle, left_trigger*, right_trigger*)
        RegisterSymbol("libScePad", "scePadGetTriggerEffect", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            guest_addr_t left_ptr = args.arg2;
            guest_addr_t right_ptr = args.arg3;
            (void)left_ptr;
            (void)right_ptr;

            LOG_DEBUG(HLE, "scePadGetTriggerEffect(handle: 0x%X) called", handle);

            return 0; // SCE_OK
        });

        // scePadGetInfo
        // Signature: scePadGetInfo(port, info*)
        RegisterSymbol("libScePad", "scePadGetInfo", [](const GuestArgs& args) -> u64 {
            u32 port = static_cast<u32>(args.arg1);
            guest_addr_t info_ptr = args.arg2;

            LOG_DEBUG(HLE, "scePadGetInfo(port: %u, info: 0x%llx) called", port, info_ptr);

            if (info_ptr) {
                // ScePadInfo structure - zero it out (no controller connected)
                for (u32 i = 0; i < 32; i += 8) {
                    Memory::Write<u64>(info_ptr + i, 0);
                }
            }

            return 0; // SCE_OK
        });

        // scePadSetSensorMode
        // Signature: scePadSetSensorMode(handle, mode)
        RegisterSymbol("libScePad", "scePadSetSensorMode", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            u32 mode = static_cast<u32>(args.arg2);

            LOG_DEBUG(HLE, "scePadSetSensorMode(handle: 0x%X, mode: %u) called", handle, mode);

            return 0; // SCE_OK
        });

        // scePadGetSensorData
        // Signature: scePadGetSensorData(handle, sensor_type, data*)
        RegisterSymbol("libScePad", "scePadGetSensorData", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            u32 sensor_type = static_cast<u32>(args.arg2);
            guest_addr_t data_ptr = args.arg3;

            LOG_DEBUG(HLE, "scePadGetSensorData(handle: 0x%X, sensor: %u, data: 0x%llx) called", handle, sensor_type, data_ptr);

            if (data_ptr) {
                // Zero out sensor data (typically 16 bytes for accelerometer/gyro)
                for (u32 i = 0; i < 16; i += 8) {
                    Memory::Write<u64>(data_ptr + i, 0);
                }
            }

            return 0; // SCE_OK
        });

        // scePadSetPlayerIndicator
        // Signature: scePadSetPlayerIndicator(handle, indicator)
        RegisterSymbol("libScePad", "scePadSetPlayerIndicator", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            u32 indicator = static_cast<u32>(args.arg2);

            LOG_DEBUG(HLE, "scePadSetPlayerIndicator(handle: 0x%X, indicator: %u) called", handle, indicator);

            return 0; // SCE_OK
        });

        // scePadGetPlayerIndicator
        // Signature: scePadGetPlayerIndicator(handle, indicator*)
        RegisterSymbol("libScePad", "scePadGetPlayerIndicator", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            guest_addr_t indicator_ptr = args.arg2;

            LOG_DEBUG(HLE, "scePadGetPlayerIndicator(handle: 0x%X) called", handle);

            if (indicator_ptr) {
                Memory::Write<u32>(indicator_ptr, 0);
            }

            return 0; // SCE_OK
        });

        // scePadSetMicMute
        // Signature: scePadSetMicMute(handle, mute)
        RegisterSymbol("libScePad", "scePadSetMicMute", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            u32 mute = static_cast<u32>(args.arg2);

            LOG_DEBUG(HLE, "scePadSetMicMute(handle: 0x%X, mute: %u) called", handle, mute);

            return 0; // SCE_OK
        });

        // scePadGetMicMute
        // Signature: scePadGetMicMute(handle, mute*)
        RegisterSymbol("libScePad", "scePadGetMicMute", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            guest_addr_t mute_ptr = args.arg2;

            LOG_DEBUG(HLE, "scePadGetMicMute(handle: 0x%X) called", handle);

            if (mute_ptr) {
                Memory::Write<u32>(mute_ptr, 0);
            }

            return 0; // SCE_OK
        });

        // scePadSetMicGain
        // Signature: scePadSetMicGain(handle, gain)
        RegisterSymbol("libScePad", "scePadSetMicGain", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            u32 gain = static_cast<u32>(args.arg2);

            LOG_DEBUG(HLE, "scePadSetMicGain(handle: 0x%X, gain: %u) called", handle, gain);

            return 0; // SCE_OK
        });

        // scePadGetMicGain
        // Signature: scePadGetMicGain(handle, gain*)
        RegisterSymbol("libScePad", "scePadGetMicGain", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            guest_addr_t gain_ptr = args.arg2;

            LOG_DEBUG(HLE, "scePadGetMicGain(handle: 0x%X) called", handle);

            if (gain_ptr) {
                Memory::Write<u32>(gain_ptr, 0);
            }

            return 0; // SCE_OK
        });

        // scePadSetHeadsetVolume
        // Signature: scePadSetHeadsetVolume(handle, volume)
        RegisterSymbol("libScePad", "scePadSetHeadsetVolume", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            u32 volume = static_cast<u32>(args.arg2);

            LOG_DEBUG(HLE, "scePadSetHeadsetVolume(handle: 0x%X, volume: %u) called", handle, volume);

            return 0; // SCE_OK
        });

        // scePadGetHeadsetVolume
        // Signature: scePadGetHeadsetVolume(handle, volume*)
        RegisterSymbol("libScePad", "scePadGetHeadsetVolume", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            guest_addr_t volume_ptr = args.arg2;

            LOG_DEBUG(HLE, "scePadGetHeadsetVolume(handle: 0x%X) called", handle);

            if (volume_ptr) {
                Memory::Write<u32>(volume_ptr, 0);
            }

            return 0; // SCE_OK
        });

        // scePadSetSpeakerVolume
        // Signature: scePadSetSpeakerVolume(handle, volume)
        RegisterSymbol("libScePad", "scePadSetSpeakerVolume", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            u32 volume = static_cast<u32>(args.arg2);

            LOG_DEBUG(HLE, "scePadSetSpeakerVolume(handle: 0x%X, volume: %u) called", handle, volume);

            return 0; // SCE_OK
        });

        // scePadGetSpeakerVolume
        // Signature: scePadGetSpeakerVolume(handle, volume*)
        RegisterSymbol("libScePad", "scePadGetSpeakerVolume", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            guest_addr_t volume_ptr = args.arg2;

            LOG_DEBUG(HLE, "scePadGetSpeakerVolume(handle: 0x%X) called", handle);

            if (volume_ptr) {
                Memory::Write<u32>(volume_ptr, 0);
            }

            return 0; // SCE_OK
        });

        // scePadSetTouchpadSensitivity
        // Signature: scePadSetTouchpadSensitivity(handle, sensitivity)
        RegisterSymbol("libScePad", "scePadSetTouchpadSensitivity", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            u32 sensitivity = static_cast<u32>(args.arg2);

            LOG_DEBUG(HLE, "scePadSetTouchpadSensitivity(handle: 0x%X, sensitivity: %u) called", handle, sensitivity);

            return 0; // SCE_OK
        });

        // scePadGetTouchpadSensitivity
        // Signature: scePadGetTouchpadSensitivity(handle, sensitivity*)
        RegisterSymbol("libScePad", "scePadGetTouchpadSensitivity", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            guest_addr_t sensitivity_ptr = args.arg2;

            LOG_DEBUG(HLE, "scePadGetTouchpadSensitivity(handle: 0x%X) called", handle);

            if (sensitivity_ptr) {
                Memory::Write<u32>(sensitivity_ptr, 0);
            }

            return 0; // SCE_OK
        });

        LOG_INFO(HLE, "libScePad HLE symbols registered successfully");
    }

} // namespace HLE