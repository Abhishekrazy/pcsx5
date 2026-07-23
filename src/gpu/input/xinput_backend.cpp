// XInput controller input backend — Windows XInput 1.4/1.3/9.1.0.
// Dynamically loads xinput1_4.dll, falls back to 1_3, then 9_1_0.

#include "input_backend.h"
#include "../../common/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <chrono>

// XInput constants (from xinput.h, but we load dynamically).
constexpr DWORD kXInputGamepadLeftThumbDeadZone  = 7849;
constexpr DWORD kXInputGamepadRightThumbDeadZone = 8689;
constexpr DWORD kXInputGamepadTriggerThreshold   = 30;

// XINPUT_GAMEPAD_* button bitmask (from xinput.h).
constexpr WORD XINPUT_GAMEPAD_DPAD_UP        = 0x0001;
constexpr WORD XINPUT_GAMEPAD_DPAD_DOWN      = 0x0002;
constexpr WORD XINPUT_GAMEPAD_DPAD_LEFT      = 0x0004;
constexpr WORD XINPUT_GAMEPAD_DPAD_RIGHT     = 0x0008;
constexpr WORD XINPUT_GAMEPAD_START          = 0x0010;
constexpr WORD XINPUT_GAMEPAD_BACK           = 0x0020;
constexpr WORD XINPUT_GAMEPAD_LEFT_THUMB     = 0x0040;
constexpr WORD XINPUT_GAMEPAD_RIGHT_THUMB    = 0x0080;
constexpr WORD XINPUT_GAMEPAD_LEFT_SHOULDER  = 0x0100;
constexpr WORD XINPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200;
constexpr WORD XINPUT_GAMEPAD_A              = 0x1000;
constexpr WORD XINPUT_GAMEPAD_B              = 0x2000;
constexpr WORD XINPUT_GAMEPAD_X              = 0x4000;
constexpr WORD XINPUT_GAMEPAD_Y              = 0x8000;

// Struct matching XINPUT_STATE (defined locally to avoid static linking).
struct XInputState {
    DWORD packet_number;
    struct {
        WORD  buttons;
        BYTE  left_trigger;
        BYTE  right_trigger;
        SHORT thumb_lx;
        SHORT thumb_ly;
        SHORT thumb_rx;
        SHORT thumb_ry;
    } gamepad;
};

struct XInputVibration {
    WORD left_motor_speed;
    WORD right_motor_speed;
};

// PS5 controller button bitmask (ScePadButton).
constexpr uint32_t kPadL2         = 0x00000100;
constexpr uint32_t kPadR2         = 0x00000200;
constexpr uint32_t kPadL1         = 0x00000400;
constexpr uint32_t kPadR1         = 0x00000800;
constexpr uint32_t kPadTriangle   = 0x00001000;
constexpr uint32_t kPadCircle     = 0x00002000;
constexpr uint32_t kPadCross      = 0x00004000;
constexpr uint32_t kPadSquare     = 0x00008000;

class XInputBackend : public InputBackend {
public:
    XInputBackend() = default;
    ~XInputBackend() override { Shutdown(); }

    bool Initialize(int controller_index) override {
        if (m_initialized) Shutdown();

        m_controller_index = controller_index;

        // Dynamically load XInput DLL.
        const char* dlls[] = {
            "xinput1_4.dll",
            "xinput1_3.dll",
            "xinput9_1_0.dll"
        };

        for (const auto& dll : dlls) {
            m_dll = ::LoadLibraryA(dll);
            if (m_dll) {
                m_get_state = reinterpret_cast<GetStateFn>(
                    ::GetProcAddress(m_dll, "XInputGetState"));
                m_set_state = reinterpret_cast<SetStateFn>(
                    ::GetProcAddress(m_dll, "XInputSetState"));
                if (m_get_state) {
                    LOG_INFO(GPU, "XInput: loaded %s", dll);
                    m_initialized = true;
                    return true;
                }
                ::FreeLibrary(m_dll);
                m_dll = nullptr;
            }
        }

        LOG_WARN(GPU, "XInput: no DLL found (xinput1_4/1_3/9_1_0)");
        return false;
    }

    void Shutdown() override {
        if (m_dll) {
            ::FreeLibrary(m_dll);
            m_dll = nullptr;
        }
        m_get_state = nullptr;
        m_set_state = nullptr;
        m_initialized = false;
    }

    bool IsInitialized() const override { return m_initialized; }

    InputCaps GetCaps() const override {
        InputCaps caps;
        caps.backend_name = "XInput";
        caps.has_rumble = true;
        caps.has_touchpad = false;
        caps.has_motion = false;
        caps.has_haptics = false;
        caps.max_controllers = 4;
        return caps;
    }

    bool Poll(ControllerState& out) override {
        if (!m_initialized || !m_get_state) return false;

        XInputState state{};
        const DWORD result = m_get_state(
            static_cast<DWORD>(m_controller_index), &state);

        if (result != ERROR_SUCCESS) {
            out = ControllerState{};
            out.connected = false;
            return false;
        }

        out = ControllerState{};
        out.connected = true;
        out.timestamp_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        const WORD& b = state.gamepad.buttons;

        // Map XInput buttons → PS5 ScePadButton bitmask.
        if (b & XINPUT_GAMEPAD_DPAD_UP)     out.buttons |= 0x00000010;
        if (b & XINPUT_GAMEPAD_DPAD_DOWN)   out.buttons |= 0x00000040;
        if (b & XINPUT_GAMEPAD_DPAD_LEFT)   out.buttons |= 0x00000080;
        if (b & XINPUT_GAMEPAD_DPAD_RIGHT)  out.buttons |= 0x00000020;

        if (b & XINPUT_GAMEPAD_A)           out.buttons |= kPadCross;
        if (b & XINPUT_GAMEPAD_B)           out.buttons |= kPadCircle;
        if (b & XINPUT_GAMEPAD_X)           out.buttons |= kPadSquare;
        if (b & XINPUT_GAMEPAD_Y)           out.buttons |= kPadTriangle;

        if (b & XINPUT_GAMEPAD_LEFT_SHOULDER)  out.buttons |= kPadL1;
        if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) out.buttons |= kPadR1;

        if (b & XINPUT_GAMEPAD_LEFT_THUMB)  out.buttons |= 0x00000002; // L3
        if (b & XINPUT_GAMEPAD_RIGHT_THUMB) out.buttons |= 0x00000004; // R3

        if (b & XINPUT_GAMEPAD_START)       out.buttons |= 0x00000008; // OPTIONS
        if (b & XINPUT_GAMEPAD_BACK)        out.buttons |= 0x00200000; // TOUCHPAD

        // Triggers → digital buttons + analog values.
        out.r2 = state.gamepad.right_trigger;
        out.l2 = state.gamepad.left_trigger;
        if (state.gamepad.left_trigger > kXInputGamepadTriggerThreshold)
            out.buttons |= kPadL2;
        if (state.gamepad.right_trigger > kXInputGamepadTriggerThreshold)
            out.buttons |= kPadR2;

        // Sticks: normalize SHORT range (-32768..32767) to u8 (0..255),
        // applying deadzone.
        auto normalize_stick =[](SHORT value, DWORD deadzone) -> uint8_t {
            if (value > -static_cast<SHORT>(deadzone) &&
                value < static_cast<SHORT>(deadzone)) {
                return 128;
            }
            // Scale to 0..255: center=128, -32768→0, 32767→255
            return static_cast<uint8_t>(
                (static_cast<int>(value) + 32768) * 255 / 65535);
        };

        out.left_x = normalize_stick(state.gamepad.thumb_lx,
                                     kXInputGamepadLeftThumbDeadZone);
        out.left_y = normalize_stick(state.gamepad.thumb_ly,
                                     kXInputGamepadLeftThumbDeadZone);
        out.right_x = normalize_stick(state.gamepad.thumb_rx,
                                      kXInputGamepadRightThumbDeadZone);
        out.right_y = normalize_stick(state.gamepad.thumb_ry,
                                      kXInputGamepadRightThumbDeadZone);

        return true;
    }

    void SetRumble(const RumbleState& rumble) override {
        if (!m_initialized || !m_set_state) return;

        XInputVibration vib{};
        vib.left_motor_speed = static_cast<WORD>(
            static_cast<uint32_t>(rumble.large_motor) * 65535 / 255);
        vib.right_motor_speed = static_cast<WORD>(
            static_cast<uint32_t>(rumble.small_motor) * 65535 / 255);

        m_set_state(static_cast<DWORD>(m_controller_index), &vib);
    }

    void SetTriggerEffect(bool /*left*/, const TriggerEffect& /*effect*/) override {
        // XInput doesn't support adaptive triggers.
    }

private:
    using GetStateFn = DWORD(WINAPI*)(DWORD, XInputState*);
    using SetStateFn = DWORD(WINAPI*)(DWORD, XInputVibration*);

    bool m_initialized = false;
    int m_controller_index = 0;
    HMODULE m_dll = nullptr;
    GetStateFn m_get_state = nullptr;
    SetStateFn m_set_state = nullptr;
};

// Factory
InputBackend* CreateXInputBackend() {
    return new XInputBackend();
}
