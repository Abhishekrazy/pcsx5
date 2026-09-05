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

#include <vector>

// hidsdi.h/hidpi.h need the Windows types and NTSTATUS in scope first.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <DualSenseWindows/IO.h>
#include "../../third_party/DualSenseWindows/src/DualSenseWindows/DS_CRC32.h"
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

    // The reader thread owns the DS5W context, but the haptics-audio path
    // needs the raw HID handle to write report 0x32, which DS5W does not model.
    // Published here under its own mutex rather than reaching into the context
    // from another thread.
    std::mutex        dev_mutex;
    void*             dev_handle = nullptr;
    bool              dev_bluetooth = false;

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
            {
                std::lock_guard<std::mutex> lk(S().dev_mutex);
                S().dev_handle = ctx._internal.deviceHandle;
                S().dev_bluetooth =
                    (ctx._internal.connection == DS5W::DeviceConnection::BT);
            }
            LOG_INFO(GPU, "DualSense: device connected (%u enumerated), transport=%s.",
                     found, S().dev_bluetooth ? "Bluetooth" : "USB");
            MarkOutputDirty();   // push current LED/rumble state to the new device
        }

        DS5W::DS5InputState in{};
        if (DS5W_FAILED(DS5W::getDeviceInputState(&ctx, &in))) {
            // Try one reconnect before giving the device up.
            if (DS5W_FAILED(DS5W::reconnectDevice(&ctx))) {
                LOG_INFO(GPU, "DualSense: device removed.");
                {
                    std::lock_guard<std::mutex> lk(S().dev_mutex);
                    S().dev_handle = nullptr;
                }
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

    {
        std::lock_guard<std::mutex> lk(S().dev_mutex);
        S().dev_handle = nullptr;
    }
    if (have_ctx) DS5W::freeDeviceContext(&ctx);
    PublishDisconnected();
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool IsBluetooth() {
    std::lock_guard<std::mutex> lk(S().dev_mutex);
    return S().dev_handle != nullptr && S().dev_bluetooth;
}

// ---------------------------------------------------------------------------
// Haptics audio over Bluetooth -- report 0x32.
//
// Written from the published protocol, not ported. DualSenseClient is GPL-3.0
// and cannot be incorporated into a GPL-2.0 project, so only the format itself
// -- report ids, offsets, encodings -- is used here. Those are facts about a
// device, not authorship.
//
// The first attempt failed in five separate ways, each recorded because each
// was a different kind of mistake:
//
//   1. 141-byte reports. The report is 142. Windows also demands writes of
//      exactly OutputReportByteLength (547 here), so the report sits at the
//      front of a full-size buffer. A 141-byte write returned
//      ERROR_INVALID_PARAMETER before the controller saw any content at all.
//   2. Unsigned PCM. The payload is *signed* s8: silence is 0, not 128.
//      Filling it with 128 was a full-scale DC offset.
//   3. The session block was 0xFE 00 00 00 00 0xFF 00, from a summary of a
//      Linux proof-of-concept. The form the controller accepts is 0xFE, five
//      0x40 buffer-length bytes, then a free-running interval counter.
//   4. No init-prime. The stream must be opened first by a 0x32 report whose
//      state block enables the audio path. Without it the controller accepts
//      every write and does nothing -- exactly what was observed: 282 reports
//      sent, TRUE returned, no vibration.
//   5. A constant where a per-report counter belongs.
//
// Layout, 142 bytes:
//   [0]        0x32
//   [1]        sequence in the high nibble
//   [2..3]     0x91 0x07   sized packet 0x11, length 7
//   [4..10]    0xFE, 0x40 x5, interval counter
//   [11..12]   0x92 0x40   sized packet 0x12, length 64
//   [13..76]   64 bytes of s8 stereo PCM at 3000 Hz (32 frames, 10.67 ms)
//   [77..137]  zero
//   [138..141] CRC32, little-endian, over [0..137]
// ---------------------------------------------------------------------------
namespace {

constexpr size_t kHapticReportSize  = 142;
constexpr size_t kHapticAudioBytes  = 64;
constexpr size_t kHapticAudioOffset = 13;
constexpr size_t kHapticCrcOffset   = kHapticReportSize - 4;
constexpr unsigned kHapticSampleRate = 3000;
constexpr unsigned kHapticChannels   = 2;

void FinishReport(u8* r) {
    const u32 crc = __DS5W::CRC32::compute(r, kHapticCrcOffset);
    r[kHapticCrcOffset + 0] = static_cast<u8>(crc);
    r[kHapticCrcOffset + 1] = static_cast<u8>(crc >> 8);
    r[kHapticCrcOffset + 2] = static_cast<u8>(crc >> 16);
    r[kHapticCrcOffset + 3] = static_cast<u8>(crc >> 24);
}

// The init-prime that opens the audio stream. Its 47-byte state block is what
// enables the audio path; haptics sent without it are accepted and ignored.
void BuildInitPrime(u8* r, u8 volume) {
    std::memset(r, 0, kHapticReportSize);
    r[0] = 0x32;
    r[1] = 0x10;
    r[2] = 0x90;
    r[3] = 0x3F;
    u8* st = r + 4;                        // 47-byte output state
    st[0]  = 0x80 | 0x20 | 0x10;           // allow audio control, speaker + headphone volume
    st[1]  = 0x80;                         // allow audio control 2
    st[4]  = volume;                       // headphone volume
    st[5]  = volume;                       // speaker volume
    st[7]  = 0x30;                         // output path: speaker
    st[37] = 0x02;                         // audio control 2
    FinishReport(r);
}

void BuildHapticsReport(u8* r, const s8* pcm, size_t len, u8 seq, u8 counter) {
    std::memset(r, 0, kHapticReportSize);
    r[0]  = 0x32;
    r[1]  = static_cast<u8>((seq & 0x0F) << 4);
    r[2]  = 0x91;
    r[3]  = 0x07;
    r[4]  = 0xFE;
    r[5] = r[6] = r[7] = r[8] = r[9] = 0x40;
    r[10] = counter;
    r[11] = 0x92;
    r[12] = static_cast<u8>(kHapticAudioBytes);
    std::memcpy(r + kHapticAudioOffset, pcm, len);
    // The remainder stays zero, which is silence for signed PCM.
    FinishReport(r);
}

} // namespace

bool PlayHapticsPcmBlocking(const u8* pcm, size_t bytes) {
    if (!pcm || bytes == 0) return false;

    void* handle = nullptr;
    bool bt = false;
    {
        std::lock_guard<std::mutex> lk(S().dev_mutex);
        handle = S().dev_handle;
        bt = S().dev_bluetooth;
    }
    if (!handle) { LOG_WARN(GPU, "DualSense haptics: no open device."); return false; }
    if (!bt) {
        LOG_WARN(GPU, "DualSense haptics: device is on USB, where the audio "
                      "endpoints apply instead of report 0x32.");
        return false;
    }

    USHORT out_len = 0;
    {
        PHIDP_PREPARSED_DATA pp = nullptr;
        if (::HidD_GetPreparsedData(reinterpret_cast<HANDLE>(handle), &pp) && pp) {
            HIDP_CAPS caps{};
            if (::HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                out_len = caps.OutputReportByteLength;
            }
            ::HidD_FreePreparsedData(pp);
        }
    }
    if (out_len < kHapticReportSize) {
        LOG_WARN(GPU, "DualSense haptics: device output report length %u is too "
                      "small for the %zu-byte haptics report.",
                 static_cast<unsigned>(out_len), kHapticReportSize);
        return false;
    }

    std::vector<u8> wire(out_len, 0);
    auto write_wire = [&]() -> bool {
        DWORD written = 0;
        const BOOL ok = ::WriteFile(reinterpret_cast<HANDLE>(handle), wire.data(),
                                    static_cast<DWORD>(out_len), &written, nullptr);
        return ok && written == out_len;
    };

    BuildInitPrime(wire.data(), 0x40);
    if (!write_wire()) {
        LOG_WARN(GPU, "DualSense haptics: init-prime write failed (error %lu).",
                 static_cast<unsigned long>(::GetLastError()));
        return false;
    }

    const auto frame_period = std::chrono::microseconds(
        1000000ull * (kHapticAudioBytes / kHapticChannels) / kHapticSampleRate);

    u8 seq = 0, counter = 0;
    auto next = std::chrono::steady_clock::now();

    const s8 silence[kHapticAudioBytes] = {};
    for (int i = 0; i < 8; ++i) {
        BuildHapticsReport(wire.data(), silence, kHapticAudioBytes, seq, counter++);
        seq = static_cast<u8>((seq + 1) & 0x0F);
        if (!write_wire()) {
            LOG_WARN(GPU, "DualSense haptics: preroll write failed (error %lu).",
                     static_cast<unsigned long>(::GetLastError()));
            return false;
        }
        next += frame_period;
        std::this_thread::sleep_until(next);
    }

    const s8* samples = reinterpret_cast<const s8*>(pcm);
    size_t offset = 0, sent = 0;
    while (offset < bytes) {
        const size_t chunk = (bytes - offset) < kHapticAudioBytes
                                 ? (bytes - offset) : kHapticAudioBytes;
        BuildHapticsReport(wire.data(), samples + offset, chunk, seq, counter++);
        seq = static_cast<u8>((seq + 1) & 0x0F);
        if (!write_wire()) {
            LOG_WARN(GPU, "DualSense haptics: write failed after %zu report(s) "
                          "(error %lu).", sent,
                     static_cast<unsigned long>(::GetLastError()));
            return false;
        }
        ++sent;
        offset += chunk;
        next += frame_period;
        std::this_thread::sleep_until(next);
    }

    LOG_INFO(GPU, "DualSense haptics: primed, prerolled, sent %zu report(s) "
                  "(%zu bytes of s8 PCM).", sent, bytes);
    return true;
}

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
