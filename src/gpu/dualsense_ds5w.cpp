// DualSense controller implementation, built on the vendored DualSenseWindows
// library (third_party/DualSenseWindows, MIT, by Ludwig Fuechsl).
//
// Why this replaced the previous in-header implementation:
//
//  * State ownership.  The old reader kept ~30 namespace-scope `static`
//    globals in a header, giving every including translation unit its own
//    device handle, output state and reader thread.  Output calls from one TU
//    therefore wrote to a handle a different TU had opened, which is why
//    rumble, lightbar and the mic LED did nothing.  All state now lives here,
//    in one TU, behind one mutex.
//
//  * Report layouts.  The old reader guessed report offsets at runtime and
//    scored them with a heuristic (`g_last_layout`, `g_layout_streak`,
//    `g_b0_pos`).  When it guessed wrong the sticks read as centred.
//    DualSenseWindows knows the USB and Bluetooth layouts, including the
//    Bluetooth CRC32 the controller requires on output reports.
//
// The public API and the Sample layout are unchanged, so libscepad,
// vulkan_backend and the input backend needed no modification.

#include "dualsense_hid.h"
#include "../common/log.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

#include <DualSenseWindows/IO.h>
#include <DualSenseWindows/Device.h>
#include <DualSenseWindows/DS5State.h>

namespace GPU {
namespace DualSense {
namespace {

// ---------------------------------------------------------------------------
// State.  Everything below is private to this translation unit, which is the
// entire point of the file.
//
// The whole block is a deliberately leaked singleton.  A namespace-scope
// std::thread would have its destructor run at static teardown in any process
// that never calls Shutdown() -- every test binary -- and ~thread() on a still
// joinable thread calls std::terminate(), which surfaces as 0xC0000409.
// Leaking also means the reader cannot touch a destroyed mutex while the
// process is tearing down, which a bare detach() would not have prevented.
// ---------------------------------------------------------------------------
struct ReaderState {
    std::mutex        sample_mutex;
    Sample            sample;

    std::mutex        out_mutex;
    bool              out_dirty = false;
    u8                motor_large = 0, motor_small = 0;
    u8                lightbar[3] = { 0, 0, 0 };
    u8                player_leds = 0;
    bool              player_led_fade = false;
    u8                mic_led = 0;
    u8                led_brightness = 0;
    bool              leds_disabled = false;
    u8                trigger_mode[2] = { 0, 0 };
    u8                trigger_params[2][10] = {};

    std::atomic<bool> running{ false };
    std::atomic<int>  device_count{ 0 };
    std::thread*      thread = nullptr;   // owned, never destroyed at exit
};

ReaderState& S() {
    static ReaderState* s = new ReaderState();   // intentionally leaked
    return *s;
}

std::once_flag       g_start_once;

// Poll interval.  The DualSense streams at ~250 Hz on USB; 4 ms keeps latency
// low without spinning.
constexpr int kPollIntervalMs = 4;

// ---------------------------------------------------------------------------
// Mapping helpers
// ---------------------------------------------------------------------------

// Translate DS5W's button bytes into the SCE_PAD bitmask the rest of the
// emulator expects.  Bit values are preserved exactly from the previous
// implementation so no consumer changes behaviour.
u32 MapButtons(const DS5W::DS5InputState& in) {
    u32 b = 0;

    if (in.buttonsAndDpad & DS5W_ISTATE_BTX_SQUARE)   b |= 0x8000;
    if (in.buttonsAndDpad & DS5W_ISTATE_BTX_CROSS)    b |= 0x4000;
    if (in.buttonsAndDpad & DS5W_ISTATE_BTX_CIRCLE)   b |= 0x2000;
    if (in.buttonsAndDpad & DS5W_ISTATE_BTX_TRIANGLE) b |= 0x1000;

    // Low nibble is a hat value 0..7, 8 = centred.
    switch (in.buttonsAndDpad & 0x0F) {
        case 0: b |= 0x10; break;               // up
        case 1: b |= 0x10 | 0x20; break;        // up-right
        case 2: b |= 0x20; break;               // right
        case 3: b |= 0x20 | 0x40; break;        // down-right
        case 4: b |= 0x40; break;               // down
        case 5: b |= 0x40 | 0x80; break;        // down-left
        case 6: b |= 0x80; break;               // left
        case 7: b |= 0x80 | 0x10; break;        // up-left
        default: break;                         // 8 = centred
    }

    if (in.buttonsA & DS5W_ISTATE_BTN_A_LEFT_BUMPER)   b |= 0x0400;
    if (in.buttonsA & DS5W_ISTATE_BTN_A_RIGHT_BUMPER)  b |= 0x0800;
    if (in.buttonsA & DS5W_ISTATE_BTN_A_LEFT_TRIGGER)  b |= 0x0100;
    if (in.buttonsA & DS5W_ISTATE_BTN_A_RIGHT_TRIGGER) b |= 0x0200;
    if (in.buttonsA & DS5W_ISTATE_BTN_A_SELECT)        b |= 0x0001;  // Share/Create
    if (in.buttonsA & DS5W_ISTATE_BTN_A_MENU)          b |= 0x0008;  // Options
    if (in.buttonsA & DS5W_ISTATE_BTN_A_LEFT_STICK)    b |= 0x0002;  // L3
    if (in.buttonsA & DS5W_ISTATE_BTN_A_RIGHT_STICK)   b |= 0x0004;  // R3

    if (in.buttonsB & DS5W_ISTATE_BTN_B_PLAYSTATION_LOGO) b |= 0x00010000;
    if (in.buttonsB & DS5W_ISTATE_BTN_B_PAD_BUTTON)       b |= 0x00100000;

    return b;
}

// DS5W reports sticks as signed -128..127 with Y up-positive; the emulator
// expects unsigned 0..255 with Y down-positive, 128 centred.
inline u8 StickToUnsigned(char v) {
    int r = static_cast<int>(v) + 128;
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    return static_cast<u8>(r);
}

inline u8 StickToUnsignedInverted(char v) {
    int r = 127 - static_cast<int>(v);
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    return static_cast<u8>(r);
}

void CopyTouch(const DS5W::Touch& src, PadTouchPoint& dst, u8& count) {
    dst.x = static_cast<u16>(src.x);
    dst.y = static_cast<u16>(src.y);
    dst.id = static_cast<u8>(src.id);
    dst.active = src.down ? 1 : 0;
    if (src.down) ++count;
}

// Build the DS5W output report from the pending output state.
void BuildOutput(DS5W::DS5OutputState& out) {
    std::lock_guard<std::mutex> lk(S().out_mutex);

    out.leftRumble  = S().motor_large;
    out.rightRumble = S().motor_small;

    out.lightbar = DS5W::Color{ S().lightbar[0], S().lightbar[1], S().lightbar[2] };

    out.playerLeds.bitmask       = S().player_leds;
    out.playerLeds.playerLedFade = S().player_led_fade;
    out.playerLeds.brightness    = static_cast<DS5W::LedBrightness>(S().led_brightness);
    out.disableLeds              = S().leds_disabled;

    switch (S().mic_led) {
        case 1:  out.microphoneLed = DS5W::MicLed::ON; break;
        case 2:  out.microphoneLed = DS5W::MicLed::PULSE; break;
        default: out.microphoneLed = DS5W::MicLed::OFF; break;
    }

    // Trigger effects: only the modes the previous API exposed are mapped.
    // Anything else is treated as "no effect" rather than guessed at.
    DS5W::TriggerEffect* effects[2] = { &out.leftTriggerEffect, &out.rightTriggerEffect };
    for (int i = 0; i < 2; ++i) {
        const u8 mode = S().trigger_mode[i];
        const u8* p = S().trigger_params[i];
        switch (mode) {
            case 1:  // feedback
                effects[i]->effectType = DS5W::TriggerEffectType::ContinuousResitance;
                effects[i]->Continuous.startPosition = p[0];
                effects[i]->Continuous.force = p[1];
                break;
            case 2:  // weapon
                effects[i]->effectType = DS5W::TriggerEffectType::SectionResitance;
                effects[i]->Section.startPosition = p[0];
                effects[i]->Section.endPosition = p[1];
                break;
            case 3:  // vibration
                effects[i]->effectType = DS5W::TriggerEffectType::EffectEx;
                effects[i]->EffectEx.startPosition = p[0];
                effects[i]->EffectEx.keepEffect = p[1] != 0;
                effects[i]->EffectEx.beginForce = p[2];
                effects[i]->EffectEx.middleForce = p[3];
                effects[i]->EffectEx.endForce = p[4];
                effects[i]->EffectEx.frequency = p[5];
                break;
            default:
                effects[i]->effectType = DS5W::TriggerEffectType::NoResitance;
                break;
        }
    }

    S().out_dirty = false;
}

void MarkOutputDirty() {
    std::lock_guard<std::mutex> lk(S().out_mutex);
    S().out_dirty = true;
}

void PublishDisconnected() {
    std::lock_guard<std::mutex> lk(S().sample_mutex);
    S().sample = Sample{};
    S().sample.connected = false;
}

// ---------------------------------------------------------------------------
// Reader thread
// ---------------------------------------------------------------------------
void ReaderThread() {
    DS5W::DeviceContext ctx{};
    bool have_ctx = false;

    while (S().running.load(std::memory_order_relaxed)) {
        if (!have_ctx) {
            DS5W::DeviceEnumInfo infos[8]{};
            unsigned int found = 0;
            DS5W::enumDevices(infos, 8, &found);
            S().device_count.store(static_cast<int>(found), std::memory_order_relaxed);

            if (found == 0) {
                PublishDisconnected();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            // Only controller 0 is streamed today; multi-pad routing is a
            // separate, specified task.
            if (DS5W_FAILED(DS5W::initDeviceContext(&infos[0], &ctx))) {
                PublishDisconnected();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            have_ctx = true;
            LOG_INFO(GPU, "DualSense: device connected (%u enumerated).", found);
            MarkOutputDirty();   // push current LED/rumble state to the new device
        }

        DS5W::DS5InputState in{};
        if (DS5W_FAILED(DS5W::getDeviceInputState(&ctx, &in))) {
            // Try one reconnect before giving the device up.
            if (DS5W_FAILED(DS5W::reconnectDevice(&ctx))) {
                LOG_INFO(GPU, "DualSense: device removed.");
                DS5W::freeDeviceContext(&ctx);
                have_ctx = false;
                PublishDisconnected();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        Sample s;
        s.connected = true;
        s.buttons = MapButtons(in);
        s.lx = StickToUnsigned(in.leftStick.x);
        s.ly = StickToUnsignedInverted(in.leftStick.y);
        s.rx = StickToUnsigned(in.rightStick.x);
        s.ry = StickToUnsignedInverted(in.rightStick.y);
        s.l2 = in.leftTrigger;
        s.r2 = in.rightTrigger;

        s.touch_count = 0;
        CopyTouch(in.touchPoint1, s.touch[0], s.touch_count);
        CopyTouch(in.touchPoint2, s.touch[1], s.touch_count);

        s.accel[0] = static_cast<float>(in.accelerometer.x);
        s.accel[1] = static_cast<float>(in.accelerometer.y);
        s.accel[2] = static_cast<float>(in.accelerometer.z);
        s.gyro[0]  = static_cast<float>(in.gyroscope.x);
        s.gyro[1]  = static_cast<float>(in.gyroscope.y);
        s.gyro[2]  = static_cast<float>(in.gyroscope.z);

        s.battery_level       = in.battery.level;
        s.battery_charging    = in.battery.chargin;
        s.battery_full        = in.battery.fullyCharged;
        s.headphone_connected = in.headPhoneConnected;
        s.trigger_feedback[0] = in.leftTriggerFeedback;
        s.trigger_feedback[1] = in.rightTriggerFeedback;

        {
            std::lock_guard<std::mutex> lk(S().sample_mutex);
            S().sample = s;
        }

        // Output is written from this thread only, so it can never race a read.
        bool dirty;
        {
            std::lock_guard<std::mutex> lk(S().out_mutex);
            dirty = S().out_dirty;
        }
        if (dirty) {
            DS5W::DS5OutputState out{};
            BuildOutput(out);
            DS5W::setDeviceOutputState(&ctx, &out);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }

    if (have_ctx) DS5W::freeDeviceContext(&ctx);
    PublishDisconnected();
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void EnsureStarted() {
    std::call_once(g_start_once, []() {
        S().running.store(true, std::memory_order_relaxed);
        S().thread = new std::thread(ReaderThread);
        LOG_INFO(GPU, "DualSense: reader started (DualSenseWindows backend).");
    });
}

void Shutdown() {
    if (!S().running.exchange(false)) return;
    if (S().thread && S().thread->joinable()) S().thread->join();
    LOG_INFO(GPU, "DualSense: reader stopped.");
}

bool GetSample(Sample& out) {
    if (!S().running.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> lk(S().sample_mutex);
    out = S().sample;
    return true;
}

int GetDeviceCount() {
    return S().device_count.load(std::memory_order_relaxed);
}

void SetRumble(u8 large_motor, u8 small_motor) {
    {
        std::lock_guard<std::mutex> lk(S().out_mutex);
        S().motor_large = large_motor;
        S().motor_small = small_motor;
    }
    MarkOutputDirty();
}

void SetTriggerEffect(bool left, u8 mode, const u8 params[10]) {
    {
        std::lock_guard<std::mutex> lk(S().out_mutex);
        const int i = left ? 0 : 1;
        S().trigger_mode[i] = mode;
        if (params) std::memcpy(S().trigger_params[i], params, 10);
        else        std::memset(S().trigger_params[i], 0, 10);
    }
    MarkOutputDirty();
}

void SetLightBar(u8 r, u8 g, u8 b) {
    {
        std::lock_guard<std::mutex> lk(S().out_mutex);
        S().lightbar[0] = r; S().lightbar[1] = g; S().lightbar[2] = b;
    }
    MarkOutputDirty();
}

void SetPlayerLeds(u8 bitmask, bool fade) {
    {
        std::lock_guard<std::mutex> lk(S().out_mutex);
        S().player_leds = bitmask;
        S().player_led_fade = fade;
    }
    MarkOutputDirty();
}

void SetMicLed(u8 mode) {
    {
        std::lock_guard<std::mutex> lk(S().out_mutex);
        S().mic_led = mode;
    }
    MarkOutputDirty();
}

void SetLedOptions(u8 brightness, bool disabled) {
    {
        std::lock_guard<std::mutex> lk(S().out_mutex);
        S().led_brightness = brightness;
        S().leds_disabled = disabled;
    }
    MarkOutputDirty();
}

} // namespace DualSense
} // namespace GPU
