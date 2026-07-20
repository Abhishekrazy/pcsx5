#include "hle.h"
#include "../common/log.h"
#include "../gpu/gpu.h"
#include "../memory/memory.h"

namespace HLE {

    void RegisterLibScePad() {
        LOG_INFO(HLE, "Registering libScePad HLE symbols...");

        // NOTE: scePadInit/Open/OpenExt/Close/Read/ReadState/GetHandle are
        // implemented in libpad.cpp (registered earlier; RegisterSymbol is
        // last-registration-win, so re-registering them here would shadow
        // the real implementations).  Only ancillary stubs live here.

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

        // scePadSetVibration(handle, const ScePadVibrationParam* param)
        // ScePadVibrationParam: { uint8_t largeMotor; uint8_t smallMotor; }
        // Routes to the primary XInput controller's rumble motors.
        RegisterSymbol("libScePad", "scePadSetVibration", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            guest_addr_t param_ptr = args.arg2;

            LOG_DEBUG(HLE, "scePadSetVibration(handle: 0x%X, param: 0x%llx) called", handle, param_ptr);

            if (!param_ptr) {
                return 0x80020003; // ORBIS_GEN2_ERROR_INVALID_ARGUMENT
            }
            u8 large_motor = Memory::Read<u8>(param_ptr);
            u8 small_motor = Memory::Read<u8>(param_ptr + 1);
            GPU::SetPadVibration(large_motor, small_motor);
            return 0; // SCE_OK
        });
        RegisterSymbol("libkernel", "yFVnOdGxvZY#L#M", [](const GuestArgs& args) -> u64 {
            guest_addr_t param_ptr = args.arg2;
            if (!param_ptr) {
                return 0x80020003; // ORBIS_GEN2_ERROR_INVALID_ARGUMENT
            }
            GPU::SetPadVibration(Memory::Read<u8>(param_ptr), Memory::Read<u8>(param_ptr + 1));
            return 0;
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

        // scePadSetTriggerEffect(handle, ScePadTriggerEffectParam* param)
        // ScePadTriggerEffectParam (retail layout):
        //   0x00 u8 triggerMask (bit0 = L2, bit1 = R2)
        //   0x01 u8 padding[7]
        //   0x08 command[0] (L2): u8 mode, u8 paramData[10]
        //   0x13 command[1] (R2): u8 mode, u8 paramData[10]
        // Modes: 0=off, 1=feedback, 2=weapon, 3=vibration — identical to the
        // DualSense HID trigger-effect mode bytes for these four.
        RegisterSymbol("libScePad", "scePadSetTriggerEffect", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            guest_addr_t param_ptr = args.arg2;

            LOG_DEBUG(HLE, "scePadSetTriggerEffect(handle: 0x%X, param: 0x%llx) called", handle, param_ptr);

            if (handle != 0 && handle != 1) { // primary pad handles (see libpad.cpp)
                return 0x80920003; // SCE_PAD_ERROR_INVALID_HANDLE
            }
            if (!param_ptr) {
                return 0x80020003; // ORBIS_GEN2_ERROR_INVALID_ARGUMENT
            }

            const u8 mask = Memory::Read<u8>(param_ptr);
            for (u32 side = 0; side < 2; ++side) { // 0 = L2/left, 1 = R2/right
                if (!(mask & (1u << side))) {
                    continue;
                }
                const guest_addr_t cmd = param_ptr + 0x08 + side * 0x0B;
                const u8 mode = Memory::Read<u8>(cmd);
                u8 params[10];
                for (u32 i = 0; i < 10; ++i) {
                    params[i] = Memory::Read<u8>(cmd + 1 + i);
                }
                GPU::SetPadAdaptiveTrigger(side == 0, mode, params);
            }
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

        // Neutral motion-sensor sample shared by scePadGetSensorData and
        // scePadGetMotionSensorData: gyro at rest (all zeros) and 1g along
        // -Y (pad held level, DualSense right-handed convention).  Real
        // motion data needs the DualSense HID path — out of scope.
        auto WriteNeutralMotion = [](guest_addr_t data_ptr) {
            const float gyro[3]  = { 0.0f, 0.0f, 0.0f };
            const float accel[3] = { 0.0f, -1.0f, 0.0f };
            Memory::WriteBuffer(data_ptr, gyro, sizeof(gyro));
            Memory::WriteBuffer(data_ptr + sizeof(gyro), accel, sizeof(accel));
        };

        // scePadGetMotionSensorData(handle, ScePadMotionSensorData* data)
        // Struct: { float angularVelocity[3]; float acceleration[3]; u64 timestamp; }
        RegisterSymbol("libScePad", "scePadGetMotionSensorData", [WriteNeutralMotion](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            guest_addr_t data_ptr = args.arg2;

            LOG_DEBUG(HLE, "scePadGetMotionSensorData(handle: 0x%X, data: 0x%llx) called", handle, data_ptr);

            if (!data_ptr) {
                return 0x80020003; // ORBIS_GEN2_ERROR_INVALID_ARGUMENT
            }
            WriteNeutralMotion(data_ptr);
            const u64 ts = 0; // unknown sample time; neutral
            Memory::WriteBuffer(data_ptr + 0x18, &ts, sizeof(ts));
            return 0; // SCE_OK
        });

        // scePadGetSensorData
        // Signature: scePadGetSensorData(handle, sensor_type, data*)
        RegisterSymbol("libScePad", "scePadGetSensorData", [WriteNeutralMotion](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            u32 sensor_type = static_cast<u32>(args.arg2);
            guest_addr_t data_ptr = args.arg3;

            LOG_DEBUG(HLE, "scePadGetSensorData(handle: 0x%X, sensor: %u, data: 0x%llx) called", handle, sensor_type, data_ptr);

            if (data_ptr) {
                // 24 bytes: angular velocity (gyro) + linear acceleration
                WriteNeutralMotion(data_ptr);
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