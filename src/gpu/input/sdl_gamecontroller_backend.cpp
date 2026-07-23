// SDL GameController input backend — cross-platform gamepad via SDL2.
// Dynamically loads SDL2.dll, uses SDL_GameController API for full
// button/axis mapping with hotplug support.

#include "input_backend.h"
#include "../../common/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cstring>
#include <vector>

// Minimal SDL type definitions (loaded dynamically, no SDL headers needed).
typedef int                 SDL_bool;
typedef unsigned int        SDL_Uint32;
typedef signed short        SDL_Sint16;
struct SDL_JoystickGUID { char data[16]; };

// SDL_GameControllerBindType
enum { SDL_CONTROLLER_BINDTYPE_NONE, SDL_CONTROLLER_BINDTYPE_BUTTON,
       SDL_CONTROLLER_BINDTYPE_AXIS, SDL_CONTROLLER_BINDTYPE_HAT };

// SDL_GameControllerAxis
enum { SDL_CONTROLLER_AXIS_INVALID = -1, SDL_CONTROLLER_AXIS_LEFTX,
       SDL_CONTROLLER_AXIS_LEFTY, SDL_CONTROLLER_AXIS_RIGHTX,
       SDL_CONTROLLER_AXIS_RIGHTY, SDL_CONTROLLER_AXIS_TRIGGERLEFT,
       SDL_CONTROLLER_AXIS_TRIGGERRIGHT, SDL_CONTROLLER_AXIS_MAX };

// SDL_GameControllerButton
enum { SDL_CONTROLLER_BUTTON_INVALID = -1, SDL_CONTROLLER_BUTTON_A,
       SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X,
       SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_BACK,
       SDL_CONTROLLER_BUTTON_GUIDE, SDL_CONTROLLER_BUTTON_START,
       SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
       SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
       SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
       SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
       SDL_CONTROLLER_BUTTON_MAX };

#define SDL_INIT_GAMECONTROLLER 0x00000020u
#define SDL_QUERY -1
#define SDL_ENABLE 1

// Function pointer types.
using SDL_InitFn           = int(*)(SDL_Uint32);
using SDL_QuitFn           = void(*)();
using SDL_NumJoysticksFn   = int(*)();
using SDL_IsGameControllerFn = SDL_bool(*)(int);
using SDL_GameControllerOpenFn = void*(*)(int);
using SDL_GameControllerCloseFn = void(*)(void*);
using SDL_GameControllerGetPadStateFn = SDL_bool(*)(void*);
using SDL_GameControllerGetButtonFn = SDL_Sint16(*)(void*, int);
using SDL_GameControllerGetAxisFn = SDL_Sint16(*)(void*, int);
using SDL_GameControllerNameForIndexFn = const char*(*)(int);
using SDL_GameControllerRumbleFn = int(*)(void*, SDL_Uint32, SDL_Uint32, SDL_Uint32);

class SdlGameControllerBackend : public InputBackend {
public:
    SdlGameControllerBackend() = default;
    ~SdlGameControllerBackend() override { Shutdown(); }

    bool Initialize(int controller_index) override;
    void Shutdown() override;
    bool IsInitialized() const override { return m_initialized; }
    InputCaps GetCaps() const override;
    bool Poll(ControllerState& out) override;
    void SetRumble(const RumbleState& rumble) override;
    void SetTriggerEffect(bool /*left*/, const TriggerEffect& /*effect*/) override {}

private:
    bool LoadSdl();
    uint8_t MapAxis(SDL_Sint16 raw);  // -32768..32767 -> 0..255

    HMODULE m_dll = nullptr;
    SDL_InitFn m_init = nullptr;
    SDL_QuitFn m_quit = nullptr;
    SDL_NumJoysticksFn m_num_joysticks = nullptr;
    SDL_IsGameControllerFn m_is_controller = nullptr;
    SDL_GameControllerOpenFn m_open = nullptr;
    SDL_GameControllerCloseFn m_close = nullptr;
    SDL_GameControllerGetPadStateFn m_get_pad = nullptr;
    SDL_GameControllerGetButtonFn m_get_btn = nullptr;
    SDL_GameControllerGetAxisFn m_get_axis = nullptr;
    SDL_GameControllerNameForIndexFn m_name_for_idx = nullptr;
    SDL_GameControllerRumbleFn m_rumble = nullptr;

    void* m_controller = nullptr;
    int m_controller_index = 0;
    bool m_initialized = false;
};

bool SdlGameControllerBackend::LoadSdl() {
    if (m_dll) return true;
    const char* dlls[] = {"SDL2.dll", "SDL.dll", nullptr};
    for (int i = 0; dlls[i]; ++i) {
        m_dll = ::LoadLibraryA(dlls[i]);
        if (!m_dll) continue;
        // Load function pointers with explicit casts (no decltype on member).
        auto Resolve = [&](const char* name, auto& out) {
            out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(
                ::GetProcAddress(m_dll, name));
        };
        Resolve("SDL_Init", m_init); Resolve("SDL_Quit", m_quit);
        Resolve("SDL_NumJoysticks", m_num_joysticks);
        Resolve("SDL_IsGameController", m_is_controller);
        Resolve("SDL_GameControllerOpen", m_open);
        Resolve("SDL_GameControllerClose", m_close);
        Resolve("SDL_GameControllerGetButton", m_get_btn);
        Resolve("SDL_GameControllerGetAxis", m_get_axis);
        Resolve("SDL_GameControllerNameForIndex", m_name_for_idx);
        Resolve("SDL_GameControllerRumble", m_rumble);
        m_get_pad = reinterpret_cast<SDL_GameControllerGetPadStateFn>(
            ::GetProcAddress(m_dll, "SDL_GameControllerGetAttached"));
        if (m_init && m_open && m_close && m_get_btn && m_get_axis) {
            LOG_INFO(GPU, "SDL GameController: loaded %s", dlls[i]);
            return true;
        }
        ::FreeLibrary(m_dll); m_dll = nullptr;
    }
    return false;
}

bool SdlGameControllerBackend::Initialize(int controller_index) {
    if (m_initialized) Shutdown();
    if (!LoadSdl()) return false;

    // Find a game controller at the requested index.
    int found = -1, ctr = 0;
    int num = m_num_joysticks ? m_num_joysticks() : 0;
    for (int i = 0; i < num; ++i) {
        if (m_is_controller && m_is_controller(i)) {
            if (ctr == controller_index) { found = i; break; }
            ++ctr;
        }
    }
    if (found < 0) {
        LOG_WARN(GPU, "SDL GameController: controller %d not found", controller_index);
        return false;
    }

    m_controller = m_open(found);
    if (!m_controller) {
        LOG_WARN(GPU, "SDL GameController: open failed");
        return false;
    }

    m_controller_index = controller_index;
    m_initialized = true;
    LOG_INFO(GPU, "SDL GameController: opened %s",
             m_name_for_idx ? m_name_for_idx(found) : "?");
    return true;
}

void SdlGameControllerBackend::Shutdown() {
    if (m_controller && m_close) m_close(m_controller);
    m_controller = nullptr;
    m_initialized = false;
}

InputCaps SdlGameControllerBackend::GetCaps() const {
    InputCaps caps;
    caps.backend_name = "SDL GameController";
    caps.has_rumble = (m_rumble != nullptr);
    caps.has_touchpad = false;
    caps.has_motion = false;
    caps.has_haptics = false;
    caps.max_controllers = 4;
    return caps;
}

uint8_t SdlGameControllerBackend::MapAxis(SDL_Sint16 raw) {
    return static_cast<uint8_t>((static_cast<int>(raw) + 32768) * 255 / 65535);
}

bool SdlGameControllerBackend::Poll(ControllerState& out) {
    if (!m_initialized || !m_controller) return false;
    if (m_get_pad && !m_get_pad(m_controller)) {
        out = ControllerState{}; return false;
    }

    out = ControllerState{};
    out.connected = true;
    out.timestamp_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    // Digital buttons.
    if (m_get_btn(m_controller, SDL_CONTROLLER_BUTTON_A))              out.buttons |= 0x00004000; // Cross
    if (m_get_btn(m_controller, SDL_CONTROLLER_BUTTON_B))              out.buttons |= 0x00002000; // Circle
    if (m_get_btn(m_controller, SDL_CONTROLLER_BUTTON_X))              out.buttons |= 0x00008000; // Square
    if (m_get_btn(m_controller, SDL_CONTROLLER_BUTTON_Y))              out.buttons |= 0x00001000; // Triangle
    if (m_get_btn(m_controller, SDL_CONTROLLER_BUTTON_DPAD_UP))       out.buttons |= 0x00000010;
    if (m_get_btn(m_controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN))     out.buttons |= 0x00000040;
    if (m_get_btn(m_controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT))     out.buttons |= 0x00000080;
    if (m_get_btn(m_controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))    out.buttons |= 0x00000020;
    if (m_get_btn(m_controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  out.buttons |= 0x00000400; // L1
    if (m_get_btn(m_controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) out.buttons |= 0x00000800; // R1
    if (m_get_btn(m_controller, SDL_CONTROLLER_BUTTON_LEFTSTICK))     out.buttons |= 0x00000002; // L3
    if (m_get_btn(m_controller, SDL_CONTROLLER_BUTTON_RIGHTSTICK))    out.buttons |= 0x00000004; // R3
    if (m_get_btn(m_controller, SDL_CONTROLLER_BUTTON_START))         out.buttons |= 0x00000008; // Options
    if (m_get_btn(m_controller, SDL_CONTROLLER_BUTTON_BACK))          out.buttons |= 0x00200000; // Touchpad
    if (m_get_btn(m_controller, SDL_CONTROLLER_BUTTON_GUIDE))         out.buttons |= 0x00010000; // PS

    // Analog axes.
    out.left_x  = MapAxis(m_get_axis(m_controller, SDL_CONTROLLER_AXIS_LEFTX));
    out.left_y  = MapAxis(m_get_axis(m_controller, SDL_CONTROLLER_AXIS_LEFTY));
    out.right_x = MapAxis(m_get_axis(m_controller, SDL_CONTROLLER_AXIS_RIGHTX));
    out.right_y = MapAxis(m_get_axis(m_controller, SDL_CONTROLLER_AXIS_RIGHTY));
    out.l2      = MapAxis(m_get_axis(m_controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
    out.r2      = MapAxis(m_get_axis(m_controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
    if (out.l2 > 30) out.buttons |= 0x00000100; // L2 digital
    if (out.r2 > 30) out.buttons |= 0x00000200; // R2 digital

    return true;
}

void SdlGameControllerBackend::SetRumble(const RumbleState& rumble) {
    if (m_controller && m_rumble) {
        m_rumble(m_controller,
                 static_cast<SDL_Uint32>(rumble.large_motor) * 65535 / 255,
                 static_cast<SDL_Uint32>(rumble.small_motor) * 65535 / 255,
                 500);  // 500 ms duration
    }
}

// Factory
InputBackend* CreateSdlGameControllerBackend() {
    return new SdlGameControllerBackend();
}
