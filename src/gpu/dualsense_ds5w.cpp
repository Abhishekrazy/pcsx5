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
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
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

#include <opus.h>
#include <algorithm>
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
// One connected controller.  Everything that used to be a single global lives
// here once per slot, so two pads never share a sample, a handle or an output
// state.  `path` is the stable key: a pad keeps its slot for as long as it
// stays connected, and re-enumeration matches by path, so unplugging pad 1
// does not renumber pad 2.
struct PadSlot {
    std::mutex        sample_mutex;
    Sample            sample;

    // The reader thread owns the DS5W context; the audio paths need the raw
    // HID handle to write reports 0x32/0x35, which DS5W does not model, so it
    // is published here under its own mutex rather than reached into.
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

    // Reader-thread-only: never touched from another thread.
    DS5W::DeviceContext ctx{};
    bool              bound = false;
    std::wstring      path;
};

struct ReaderState {
    // Speaker levels used by the Bluetooth audio init-prime, shared by every
    // pad.  Held here rather than hardcoded because the right values are a
    // matter for the ear: a sweep of (0x50,0x02), (0xFF,0x02) and (0xFF,0x03)
    // was played to the user, 0xFF matched PS5-like loudness, and raising the
    // preamp past 0x02 made no audible difference.  Volume is the control that
    // matters; the preamp is not worth pushing.
    std::mutex        audio_level_mutex;
    u8                speaker_volume = 0xFF;
    u8                preamp_gain    = 0x02;

    PadSlot           slots[kMaxPads];

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

// Build the DS5W output report from a slot's pending output state.
void BuildOutput(PadSlot& p, DS5W::DS5OutputState& out) {
    std::lock_guard<std::mutex> lk(p.out_mutex);

    out.leftRumble  = p.motor_large;
    out.rightRumble = p.motor_small;

    out.lightbar = DS5W::Color{ p.lightbar[0], p.lightbar[1], p.lightbar[2] };

    out.playerLeds.bitmask       = p.player_leds;
    out.playerLeds.playerLedFade = p.player_led_fade;
    out.playerLeds.brightness    = static_cast<DS5W::LedBrightness>(p.led_brightness);
    out.disableLeds              = p.leds_disabled;

    switch (p.mic_led) {
        case 1:  out.microphoneLed = DS5W::MicLed::ON; break;
        case 2:  out.microphoneLed = DS5W::MicLed::PULSE; break;
        default: out.microphoneLed = DS5W::MicLed::OFF; break;
    }

    // Trigger effects: only the modes the previous API exposed are mapped.
    // Anything else is treated as "no effect" rather than guessed at.
    DS5W::TriggerEffect* effects[2] = { &out.leftTriggerEffect, &out.rightTriggerEffect };
    for (int i = 0; i < 2; ++i) {
        const u8 mode = p.trigger_mode[i];
        const u8* q = p.trigger_params[i];
        switch (mode) {
            case 1:  // feedback
                effects[i]->effectType = DS5W::TriggerEffectType::ContinuousResitance;
                effects[i]->Continuous.startPosition = q[0];
                effects[i]->Continuous.force = q[1];
                break;
            case 2:  // weapon
                effects[i]->effectType = DS5W::TriggerEffectType::SectionResitance;
                effects[i]->Section.startPosition = q[0];
                effects[i]->Section.endPosition = q[1];
                break;
            case 3:  // vibration
                effects[i]->effectType = DS5W::TriggerEffectType::EffectEx;
                effects[i]->EffectEx.startPosition = q[0];
                effects[i]->EffectEx.keepEffect = q[1] != 0;
                effects[i]->EffectEx.beginForce = q[2];
                effects[i]->EffectEx.middleForce = q[3];
                effects[i]->EffectEx.endForce = q[4];
                effects[i]->EffectEx.frequency = q[5];
                break;
            default:
                effects[i]->effectType = DS5W::TriggerEffectType::NoResitance;
                break;
        }
    }

    p.out_dirty = false;
}

void MarkOutputDirty(PadSlot& p) {
    std::lock_guard<std::mutex> lk(p.out_mutex);
    p.out_dirty = true;
}

void PublishDisconnected(PadSlot& p) {
    std::lock_guard<std::mutex> lk(p.sample_mutex);
    p.sample = Sample{};
    p.sample.connected = false;
}

// Resolve an index to its slot, or null for an index out of range.  Callers
// that need a *connected* pad check the sample or handle themselves; an empty
// slot is a valid thing to ask about (it answers "not connected").
PadSlot* SlotAt(int index) {
    if (index < 0 || index >= kMaxPads) return nullptr;
    return &S().slots[index];
}

// Release a slot's device and mark it free.  Reader thread only.
void UnbindSlot(PadSlot& p, const char* why) {
    LOG_INFO(GPU, "DualSense: pad %d %s.", static_cast<int>(&p - S().slots), why);
    {
        std::lock_guard<std::mutex> lk(p.dev_mutex);
        p.dev_handle = nullptr;
    }
    if (p.bound) DS5W::freeDeviceContext(&p.ctx);
    p.bound = false;
    p.path.clear();
    PublishDisconnected(p);
}

// ---------------------------------------------------------------------------
// Reader thread
// ---------------------------------------------------------------------------
// Enumerate and bind.  A pad that is already bound keeps its slot (matched by
// path); a new pad takes the first free slot.  Returns how many are bound.
int RefreshBindings() {
    DS5W::DeviceEnumInfo infos[kMaxPads]{};
    unsigned int found = 0;
    DS5W::enumDevices(infos, kMaxPads, &found);
    S().device_count.store(static_cast<int>(found), std::memory_order_relaxed);

    for (unsigned int i = 0; i < found; ++i) {
        const std::wstring path = infos[i]._internal.path;
        bool already = false;
        for (auto& p : S().slots) {
            if (p.bound && p.path == path) { already = true; break; }
        }
        if (already) continue;

        PadSlot* free_slot = nullptr;
        for (auto& p : S().slots) {
            if (!p.bound) { free_slot = &p; break; }
        }
        if (!free_slot) break;   // every slot taken

        PadSlot& p = *free_slot;
        if (DS5W_FAILED(DS5W::initDeviceContext(&infos[i], &p.ctx))) {
            continue;   // try again on the next refresh
        }
        p.bound = true;
        p.path  = path;
        {
            std::lock_guard<std::mutex> lk(p.dev_mutex);
            p.dev_handle    = p.ctx._internal.deviceHandle;
            p.dev_bluetooth = (p.ctx._internal.connection == DS5W::DeviceConnection::BT);
        }
        LOG_INFO(GPU, "DualSense: pad %d connected (%u enumerated), transport=%s.",
                 static_cast<int>(&p - S().slots), found,
                 p.dev_bluetooth ? "Bluetooth" : "USB");
        MarkOutputDirty(p);   // push current LED/rumble state to the new device
    }

    int bound = 0;
    for (auto& p : S().slots) bound += p.bound ? 1 : 0;
    return bound;
}

// One input read, publish, and output write for a bound slot.  Returns false
// if the device went away and the slot was released.
bool ServiceSlot(PadSlot& p) {
    DS5W::DS5InputState in{};
    if (DS5W_FAILED(DS5W::getDeviceInputState(&p.ctx, &in))) {
        // Try one reconnect before giving the device up.
        if (DS5W_FAILED(DS5W::reconnectDevice(&p.ctx))) {
            UnbindSlot(p, "removed");
            return false;
        }
        return true;
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

    // Status byte, read from the raw report DualSenseWindows just parsed.
    // Its evaluator is handed the buffer at [2] on Bluetooth and [1] on USB,
    // and reads the status byte at +0x35 from there.
    {
        const size_t base = (p.ctx._internal.connection == DS5W::DeviceConnection::BT) ? 2 : 1;
        const u8 status = p.ctx._internal.hidBuffer[base + 0x35];
        s.mic_jack  = (status & 0x02) != 0;
        s.mic_muted = (status & 0x04) != 0;
        s.usb_data  = (status & 0x08) != 0;
        s.usb_power = (status & 0x10) != 0;
    }
    s.headphone_connected = in.headPhoneConnected;
    s.trigger_feedback[0] = in.leftTriggerFeedback;
    s.trigger_feedback[1] = in.rightTriggerFeedback;

    {
        std::lock_guard<std::mutex> lk(p.sample_mutex);
        p.sample = s;
    }

    // Output is written from this thread only, so it can never race a read.
    bool dirty;
    {
        std::lock_guard<std::mutex> lk(p.out_mutex);
        dirty = p.out_dirty;
    }
    if (dirty) {
        DS5W::DS5OutputState out{};
        BuildOutput(p, out);
        DS5W::setDeviceOutputState(&p.ctx, &out);
    }
    return true;
}

void ReaderThread() {
    auto last_enum = std::chrono::steady_clock::now() - std::chrono::seconds(1);

    while (S().running.load(std::memory_order_relaxed)) {
        // Re-enumerate when a slot is free, but not more than twice a second:
        // enumeration walks every HID device on the system.
        int bound = 0;
        for (auto& p : S().slots) bound += p.bound ? 1 : 0;
        const auto now = std::chrono::steady_clock::now();
        if (bound < kMaxPads && now - last_enum >= std::chrono::milliseconds(500)) {
            bound = RefreshBindings();
            last_enum = now;
        }

        if (bound == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        for (auto& p : S().slots) {
            if (p.bound) ServiceSlot(p);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }

    for (auto& p : S().slots) {
        if (p.bound) UnbindSlot(p, "released at shutdown");
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void SetBluetoothAudioLevels(u8 speaker_volume, u8 preamp_gain) {
    std::lock_guard<std::mutex> lk(S().audio_level_mutex);
    S().speaker_volume = speaker_volume;
    S().preamp_gain    = preamp_gain;
}

bool IsBluetooth(int index) {
    PadSlot* p = SlotAt(index);
    if (!p) return false;
    std::lock_guard<std::mutex> lk(p->dev_mutex);
    return p->dev_handle != nullptr && p->dev_bluetooth;
}
bool IsBluetooth() { return IsBluetooth(0); }

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
void BuildInitPrime(u8* r, u8 volume, u8 preamp) {
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
    st[37] = preamp;                       // audio control 2 == preamp gain
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

bool PlayHapticsPcmBlocking(int index, const u8* pcm, size_t bytes) {
    if (!pcm || bytes == 0) return false;

    PadSlot* slot = SlotAt(index);
    if (!slot) return false;
    void* handle = nullptr;
    bool bt = false;
    {
        std::lock_guard<std::mutex> lk(slot->dev_mutex);
        handle = slot->dev_handle;
        bt = slot->dev_bluetooth;
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

    u8 vol, pre;
    {
        std::lock_guard<std::mutex> lk(S().audio_level_mutex);
        vol = S().speaker_volume;
        pre = S().preamp_gain;
    }
    BuildInitPrime(wire.data(), vol, pre);
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

// ---------------------------------------------------------------------------
// Speaker audio over Bluetooth -- report 0x35.
//
// A different lane from haptics, and a different payload: where 0x32 carries
// raw signed PCM for the voice coils, 0x35 carries a 200-byte Opus frame for
// the speaker or the headset jack. The controller holds a real Opus decoder,
// which is why libopus is vendored (ADR-002) instead of an encoder being
// written here.
//
// Layout, 334 bytes -- the same envelope as the haptics report:
//   [0]        0x35
//   [1]        sequence in the high nibble
//   [2..3]     0x91 0x07   sized packet 0x11, length 7
//   [4..10]    0xFE, 0x40 x5, interval counter
//   [11]       route: 0x93 speaker, 0x96 headset
//   [12]       200
//   [13..212]  one Opus frame, exactly 200 bytes
//   [213..329] zero
//   [330..333] CRC32, little-endian, over [0..329]
//
// The 200-byte payload is not a maximum, it is the size: 160 kbps x 10 ms is
// 1600 bits is 200 bytes, so the encoder must be CBR at that rate or the frame
// does not fill the slot. A short frame is rejected rather than padded, because
// padding would hide a misconfigured encoder behind audible-but-wrong output.
//
// The device tick is 10.667 ms (512 frames at 48 kHz) while an Opus frame at
// 48 kHz is 480 samples, so each 512-frame block is resampled to 480 before
// encoding -- which is what the reference implementations do.
// ---------------------------------------------------------------------------
namespace {

constexpr size_t kSpeakerReportSize = 334;
constexpr size_t kOpusPayloadBytes  = 200;
constexpr size_t kOpusPayloadOffset = 13;
constexpr size_t kSpeakerCrcOffset  = kSpeakerReportSize - 4;
constexpr int    kOpusSampleRate    = 48000;
constexpr int    kOpusChannels      = 2;
constexpr int    kOpusFrameSamples  = 480;   // 10 ms at 48 kHz
constexpr int    kTickFrames        = 512;   // 10.667 ms, the device audio tick
constexpr int    kOpusBitrate       = 160000;

void FinishSpeakerReport(u8* r) {
    const u32 crc = __DS5W::CRC32::compute(r, kSpeakerCrcOffset);
    r[kSpeakerCrcOffset + 0] = static_cast<u8>(crc);
    r[kSpeakerCrcOffset + 1] = static_cast<u8>(crc >> 8);
    r[kSpeakerCrcOffset + 2] = static_cast<u8>(crc >> 16);
    r[kSpeakerCrcOffset + 3] = static_cast<u8>(crc >> 24);
}

void BuildSpeakerReport(u8* r, const u8* opus, u8 seq, u8 counter, bool headset) {
    std::memset(r, 0, kSpeakerReportSize);
    r[0]  = 0x35;
    r[1]  = static_cast<u8>((seq & 0x0F) << 4);
    r[2]  = 0x91;
    r[3]  = 0x07;
    r[4]  = 0xFE;
    r[5] = r[6] = r[7] = r[8] = r[9] = 0x40;
    r[10] = counter;
    r[11] = headset ? 0x96 : 0x93;
    r[12] = static_cast<u8>(kOpusPayloadBytes);
    std::memcpy(r + kOpusPayloadOffset, opus, kOpusPayloadBytes);
    FinishSpeakerReport(r);
}

// Linear resample of one interleaved stereo block, 512 frames down to 480.
void Resample512to480(const s16* in, s16* out) {
    for (int i = 0; i < kOpusFrameSamples; ++i) {
        const double pos = static_cast<double>(i) * kTickFrames / kOpusFrameSamples;
        const int    i0  = static_cast<int>(pos);
        const int    i1  = (i0 + 1 < kTickFrames) ? i0 + 1 : kTickFrames - 1;
        const double f   = pos - i0;
        for (int c = 0; c < kOpusChannels; ++c) {
            const double a = in[i0 * kOpusChannels + c];
            const double b = in[i1 * kOpusChannels + c];
            out[i * kOpusChannels + c] = static_cast<s16>(a + (b - a) * f);
        }
    }
}

} // namespace

bool PlaySpeakerPcmBlocking(int index, const s16* pcm, size_t frames, bool headset) {
    if (!pcm || frames == 0) return false;

    PadSlot* slot = SlotAt(index);
    if (!slot) return false;
    void* handle = nullptr;
    bool bt = false;
    {
        std::lock_guard<std::mutex> lk(slot->dev_mutex);
        handle = slot->dev_handle;
        bt = slot->dev_bluetooth;
    }
    if (!handle) { LOG_WARN(GPU, "DualSense speaker: no open device."); return false; }
    if (!bt) {
        LOG_WARN(GPU, "DualSense speaker: device is on USB, where the audio "
                      "endpoints apply instead of report 0x35.");
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
    if (out_len < kSpeakerReportSize) {
        LOG_WARN(GPU, "DualSense speaker: device output report length %u is too "
                      "small for the %zu-byte audio report.",
                 static_cast<unsigned>(out_len), kSpeakerReportSize);
        return false;
    }

    int err = OPUS_OK;
    OpusEncoder* enc = ::opus_encoder_create(kOpusSampleRate, kOpusChannels,
                                             OPUS_APPLICATION_AUDIO, &err);
    if (!enc || err != OPUS_OK) {
        LOG_WARN(GPU, "DualSense speaker: opus_encoder_create failed (%s).",
                 ::opus_strerror(err));
        return false;
    }
    ::opus_encoder_ctl(enc, OPUS_SET_BITRATE(kOpusBitrate));
    ::opus_encoder_ctl(enc, OPUS_SET_VBR(0));            // CBR: the slot is fixed
    ::opus_encoder_ctl(enc, OPUS_SET_VBR_CONSTRAINT(0));
    ::opus_encoder_ctl(enc, OPUS_SET_FORCE_CHANNELS(kOpusChannels));
    ::opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

    std::vector<u8> wire(out_len, 0);
    auto write_wire = [&]() -> bool {
        DWORD written = 0;
        const BOOL ok = ::WriteFile(reinterpret_cast<HANDLE>(handle), wire.data(),
                                    static_cast<DWORD>(out_len), &written, nullptr);
        return ok && written == out_len;
    };

    // Open the stream with the same init-prime the haptics lane needs. Without
    // it the controller accepts every report and plays nothing -- the failure
    // that cost two rounds on the haptics path.
    u8 vol, pre;
    {
        std::lock_guard<std::mutex> lk(S().audio_level_mutex);
        vol = S().speaker_volume;
        pre = S().preamp_gain;
    }
    LOG_INFO(GPU, "DualSense speaker: volume=0x%02X preamp=0x%02X", vol, pre);
    BuildInitPrime(wire.data(), vol, pre);
    if (!write_wire()) {
        LOG_WARN(GPU, "DualSense speaker: init-prime write failed (error %lu).",
                 static_cast<unsigned long>(::GetLastError()));
        ::opus_encoder_destroy(enc);
        return false;
    }

    const auto tick = std::chrono::microseconds(
        1000000ll * kTickFrames / kOpusSampleRate);   // 10667 us

    u8 seq = 0, counter = 0;
    auto next = std::chrono::steady_clock::now();

    std::vector<s16> block(static_cast<size_t>(kTickFrames) * kOpusChannels, 0);
    std::vector<s16> resampled(static_cast<size_t>(kOpusFrameSamples) * kOpusChannels, 0);
    u8 opus_frame[kOpusPayloadBytes];

    auto encode_and_send = [&](bool& ok_out) -> bool {
        Resample512to480(block.data(), resampled.data());
        const int n = ::opus_encode(enc, resampled.data(), kOpusFrameSamples,
                                    opus_frame,
                                    static_cast<opus_int32>(kOpusPayloadBytes));
        if (n != static_cast<int>(kOpusPayloadBytes)) {
            LOG_WARN(GPU, "DualSense speaker: encoder produced %d bytes, not %zu. "
                          "The 48 kHz / 10 ms / %d bps CBR configuration is wrong; "
                          "a short frame would not fill the report slot.",
                     n, kOpusPayloadBytes, kOpusBitrate);
            ok_out = false;
            return false;
        }
        BuildSpeakerReport(wire.data(), opus_frame, seq, counter++, headset);
        seq = static_cast<u8>((seq + 1) & 0x0F);
        if (!write_wire()) {
            LOG_WARN(GPU, "DualSense speaker: write failed (error %lu).",
                     static_cast<unsigned long>(::GetLastError()));
            ok_out = false;
            return false;
        }
        return true;
    };

    bool ok = true;

    // Preroll silence so the decoder is running before real audio arrives.
    for (int i = 0; i < 8; ++i) {
        std::fill(block.begin(), block.end(), static_cast<s16>(0));
        if (!encode_and_send(ok)) { ::opus_encoder_destroy(enc); return false; }
        next += tick;
        std::this_thread::sleep_until(next);
    }

    size_t frame_pos = 0, sent = 0;
    while (frame_pos < frames) {
        const size_t avail = frames - frame_pos;
        const size_t take = avail < static_cast<size_t>(kTickFrames)
                                ? avail : static_cast<size_t>(kTickFrames);
        std::fill(block.begin(), block.end(), static_cast<s16>(0));
        std::memcpy(block.data(), pcm + frame_pos * kOpusChannels,
                    take * kOpusChannels * sizeof(s16));
        if (!encode_and_send(ok)) { ::opus_encoder_destroy(enc); return false; }
        ++sent;
        frame_pos += take;
        next += tick;
        std::this_thread::sleep_until(next);
    }

    ::opus_encoder_destroy(enc);
    LOG_INFO(GPU, "DualSense speaker: primed, prerolled, sent %zu Opus report(s) "
                  "to the %s.", sent, headset ? "headset" : "speaker");
    return true;
}

// ---------------------------------------------------------------------------
// Firmware info -- feature report 0x20.
//
// Offsets INFERRED from the DualSenseClient reference:
//   [0]      0x20            [1..11]  build date     [12..19] build time
//   [20..21] firmware type   [22..23] software series
//   [24..27] hardware info   [28..31] main version   [44..45] update version
//   [48..51] SBL version     [52..55] DSP version    [56..59] MCU DSP version
// ---------------------------------------------------------------------------
bool ReadFirmwareInfo(int index, FirmwareInfo& out) {
    out = FirmwareInfo{};
    PadSlot* slot = SlotAt(index);
    if (!slot) return false;
    void* handle = nullptr;
    {
        std::lock_guard<std::mutex> lk(slot->dev_mutex);
        handle = slot->dev_handle;
    }
    if (!handle) return false;

    u8 report[64] = {};
    report[0] = 0x20;
    if (!::HidD_GetFeature(reinterpret_cast<HANDLE>(handle), report, sizeof(report))) {
        LOG_WARN(GPU, "DualSense: feature report 0x20 read failed (error %lu).",
                 static_cast<unsigned long>(::GetLastError()));
        return false;
    }
    if (report[0] != 0x20) {
        LOG_WARN(GPU, "DualSense: feature report reply has id 0x%02X, not 0x20.", report[0]);
        return false;
    }

    auto u16at = [&](int o) { return static_cast<u16>(report[o] | (report[o + 1] << 8)); };
    auto u32at = [&](int o) {
        return static_cast<u32>(report[o] | (report[o + 1] << 8) |
                                (report[o + 2] << 16) | (static_cast<u32>(report[o + 3]) << 24));
    };
    std::memcpy(out.build_date, report + 1, 11);   out.build_date[11] = 0;
    std::memcpy(out.build_time, report + 12, 8);   out.build_time[8]  = 0;
    out.firmware_type   = u16at(20);
    out.software_series = u16at(22);
    out.hardware_info   = u32at(24);
    out.main_version    = u32at(28);
    out.update_version  = u16at(44);
    out.sbl_version     = u32at(48);
    out.dsp_version     = u32at(52);
    out.mcu_dsp_version = u32at(56);
    out.valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// Built-in tests. Tones are generated here so a shell need not ship PCM
// across the ABI to prove a lane works.
// ---------------------------------------------------------------------------
bool PlaySpeakerTestBlocking(int index) {
    constexpr int rate = 48000, seconds = 2;
    std::vector<s16> pcm(static_cast<size_t>(rate) * 2 * seconds);
    for (int i = 0; i < rate * seconds; ++i) {
        // 880 Hz: the frequency this speaker reproduced most clearly on
        // hardware, where 440 and 220 were inaudible.
        const double v = std::sin(2.0 * 3.14159265358979 * 880.0 * i / rate);
        const s16 b = static_cast<s16>(12000.0 * v);
        pcm[static_cast<size_t>(i) * 2 + 0] = b;
        pcm[static_cast<size_t>(i) * 2 + 1] = b;
    }
    return PlaySpeakerPcmBlocking(index, pcm.data(), pcm.size() / 2, false);
}

bool PlayHapticsTestBlocking(int index) {
    constexpr int rate = 3000, seconds = 2;
    std::vector<u8> pcm(static_cast<size_t>(rate) * 2 * seconds);
    for (int i = 0; i < rate * seconds; ++i) {
        const double v = std::sin(2.0 * 3.14159265358979 * 60.0 * i / rate);
        const u8 b = static_cast<u8>(static_cast<s8>(110.0 * v));
        pcm[static_cast<size_t>(i) * 2 + 0] = b;
        pcm[static_cast<size_t>(i) * 2 + 1] = b;
    }
    return PlayHapticsPcmBlocking(index, pcm.data(), pcm.size());
}

void EnsureStarted() {
    std::call_once(g_start_once, []() {
        S().running.store(true, std::memory_order_relaxed);
        S().thread = new std::thread(ReaderThread);
        LOG_INFO(GPU, "DualSense: reader started (DualSenseWindows backend, up to %d pads).",
                 kMaxPads);
    });
}

void Shutdown() {
    if (!S().running.exchange(false)) return;
    if (S().thread && S().thread->joinable()) S().thread->join();
    LOG_INFO(GPU, "DualSense: reader stopped.");
}

int Count() {
    if (!S().running.load(std::memory_order_relaxed)) return 0;
    int n = 0;
    for (auto& p : S().slots) {
        std::lock_guard<std::mutex> lk(p.sample_mutex);
        n += p.sample.connected ? 1 : 0;
    }
    return n;
}

int GetDeviceCount() {
    return S().device_count.load(std::memory_order_relaxed);
}

bool GetSample(int index, Sample& out) {
    if (!S().running.load(std::memory_order_relaxed)) return false;
    PadSlot* p = SlotAt(index);
    if (!p) return false;
    std::lock_guard<std::mutex> lk(p->sample_mutex);
    out = p->sample;
    return true;
}

void SetRumble(int index, u8 large_motor, u8 small_motor) {
    PadSlot* p = SlotAt(index);
    if (!p) return;
    {
        std::lock_guard<std::mutex> lk(p->out_mutex);
        p->motor_large = large_motor;
        p->motor_small = small_motor;
    }
    MarkOutputDirty(*p);
}

void SetTriggerEffect(int index, bool left, u8 mode, const u8 params[10]) {
    PadSlot* p = SlotAt(index);
    if (!p) return;
    {
        std::lock_guard<std::mutex> lk(p->out_mutex);
        const int i = left ? 0 : 1;
        p->trigger_mode[i] = mode;
        if (params) std::memcpy(p->trigger_params[i], params, 10);
        else        std::memset(p->trigger_params[i], 0, 10);
    }
    MarkOutputDirty(*p);
}

void SetLightBar(int index, u8 r, u8 g, u8 b) {
    PadSlot* p = SlotAt(index);
    if (!p) return;
    {
        std::lock_guard<std::mutex> lk(p->out_mutex);
        p->lightbar[0] = r; p->lightbar[1] = g; p->lightbar[2] = b;
    }
    MarkOutputDirty(*p);
}

void SetPlayerLeds(int index, u8 bitmask, bool fade) {
    PadSlot* p = SlotAt(index);
    if (!p) return;
    {
        std::lock_guard<std::mutex> lk(p->out_mutex);
        p->player_leds = bitmask;
        p->player_led_fade = fade;
    }
    MarkOutputDirty(*p);
}

void SetMicLed(int index, u8 mode) {
    PadSlot* p = SlotAt(index);
    if (!p) return;
    {
        std::lock_guard<std::mutex> lk(p->out_mutex);
        p->mic_led = mode;
    }
    MarkOutputDirty(*p);
}

void SetLedOptions(int index, u8 brightness, bool disabled) {
    PadSlot* p = SlotAt(index);
    if (!p) return;
    {
        std::lock_guard<std::mutex> lk(p->out_mutex);
        p->led_brightness = brightness;
        p->leds_disabled = disabled;
    }
    MarkOutputDirty(*p);
}

// Index-less forms: pad 0.  Kept so the input backends, the AGC pad HLE and
// the ABI layer above need no change to keep working with one controller.
bool GetSample(Sample& out)                                   { return GetSample(0, out); }
void SetRumble(u8 large_motor, u8 small_motor)                { SetRumble(0, large_motor, small_motor); }
void SetTriggerEffect(bool left, u8 mode, const u8 params[10]) { SetTriggerEffect(0, left, mode, params); }
void SetLightBar(u8 r, u8 g, u8 b)                            { SetLightBar(0, r, g, b); }
void SetPlayerLeds(u8 bitmask, bool fade)                     { SetPlayerLeds(0, bitmask, fade); }
void SetMicLed(u8 mode)                                       { SetMicLed(0, mode); }
void SetLedOptions(u8 brightness, bool disabled)              { SetLedOptions(0, brightness, disabled); }
bool ReadFirmwareInfo(FirmwareInfo& out)                      { return ReadFirmwareInfo(0, out); }
bool PlayHapticsPcmBlocking(const u8* pcm, size_t bytes)      { return PlayHapticsPcmBlocking(0, pcm, bytes); }
bool PlaySpeakerPcmBlocking(const s16* pcm, size_t frames, bool headset) {
    return PlaySpeakerPcmBlocking(0, pcm, frames, headset);
}
bool PlaySpeakerTestBlocking()                                { return PlaySpeakerTestBlocking(0); }
bool PlayHapticsTestBlocking()                                { return PlayHapticsTestBlocking(0); }

} // namespace DualSense
} // namespace GPU
