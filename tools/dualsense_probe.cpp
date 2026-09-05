// DualSense audio probe -- manual, hardware-only.
//
// Covers the two Bluetooth audio lanes:
//   * voice-coil haptics, report 0x32, raw signed PCM at 3 kHz
//   * speaker / headset, report 0x35, Opus at 48 kHz
//
// Input (sticks, triggers, buttons, touch) and the LED and rumble outputs were
// verified working on real hardware on 2026-09-05 and are recorded in TASKS.md.
// They are not re-tested here; doing so only buried the parts still unresolved.
//
// Deliberately NOT a CTest: it needs a controller and a person to say whether
// they heard or felt anything.
//
//     build\Release\dualsense_probe.exe                 tones
//     build\Release\dualsense_probe.exe song.wav        play a file
//     build\Release\dualsense_probe.exe song.wav 30     ...for 30 seconds
//     build\Release\dualsense_probe.exe --list          show cached game music
//
// Writes dualsense_probe.log beside the working directory.
#include "gpu/dualsense_hid.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using GPU::DualSense::Sample;

namespace {

FILE* g_log = nullptr;

void Say(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fputs(buf, stdout);
    std::fflush(stdout);
    if (g_log) { std::fputs(buf, g_log); std::fflush(g_log); }
}

void Banner(const char* what) {
    Say("\n========== %s ==========\n", what);
}

void Countdown(const char* what, int seconds) {
    Say("%s in ", what);
    for (int i = seconds; i > 0; --i) {
        Say("%d... ", i);
        std::this_thread::sleep_for(1s);
    }
    Say("NOW\n");
}

std::vector<short> SpeakerTone(double hz, unsigned seconds, double amp) {
    const unsigned rate = 48000;
    std::vector<short> pcm(static_cast<size_t>(rate) * 2 * seconds);
    for (unsigned i = 0; i < rate * seconds; ++i) {
        const double t = static_cast<double>(i) / rate;
        const double v = std::sin(2.0 * 3.14159265358979 * hz * t);
        const short b = static_cast<short>(amp * v);
        pcm[i * 2 + 0] = b;
        pcm[i * 2 + 1] = b;
    }
    return pcm;
}

// ---------------------------------------------------------------------------
// A deliberately small WAV reader: 16-bit PCM only, any rate, mono or stereo,
// converted to the 48 kHz interleaved stereo the speaker lane takes.
//
// It refuses what it cannot handle rather than guessing. A reader that quietly
// mis-parsed a header would produce noise, and noise from the wrong cause is
// worse than a clear refusal -- the whole point of this probe is to tell a
// working audio path from a broken one.
// ---------------------------------------------------------------------------
bool LoadWav48kStereo(const char* path, std::vector<short>& out, std::string& why) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) { why = "cannot open the file"; return false; }

    unsigned char hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12 ||
        std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        std::fclose(f); why = "not a RIFF/WAVE file"; return false;
    }

    unsigned short channels = 0, bits = 0, format = 0;
    unsigned rate = 0;
    std::vector<unsigned char> data;

    for (;;) {
        unsigned char ch[8];
        if (std::fread(ch, 1, 8, f) != 8) break;
        const unsigned size = ch[4] | (ch[5] << 8) | (ch[6] << 16) | (unsigned(ch[7]) << 24);
        if (std::memcmp(ch, "fmt ", 4) == 0) {
            std::vector<unsigned char> fmt(size);
            if (std::fread(fmt.data(), 1, size, f) != size) break;
            if (size >= 16) {
                format   = static_cast<unsigned short>(fmt[0] | (fmt[1] << 8));
                channels = static_cast<unsigned short>(fmt[2] | (fmt[3] << 8));
                rate     = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (unsigned(fmt[7]) << 24);
                bits     = static_cast<unsigned short>(fmt[14] | (fmt[15] << 8));
            }
        } else if (std::memcmp(ch, "data", 4) == 0) {
            data.resize(size);
            if (std::fread(data.data(), 1, size, f) != size) data.clear();
            break;
        } else {
            std::fseek(f, static_cast<long>(size + (size & 1)), SEEK_CUR);
        }
    }
    std::fclose(f);

    if (format != 1 || bits != 16) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "only 16-bit PCM is supported (found format %u, %u-bit)",
                      format, bits);
        why = buf;
        return false;
    }
    if (channels < 1 || channels > 2) { why = "only mono or stereo is supported"; return false; }
    if (data.empty() || rate == 0)    { why = "no audio data found";              return false; }

    const size_t in_frames = data.size() / (2u * channels);
    const short* src = reinterpret_cast<const short*>(data.data());

    // Resample to 48 kHz if needed, and widen mono to stereo.
    const size_t out_frames = (rate == 48000)
        ? in_frames
        : static_cast<size_t>(static_cast<double>(in_frames) * 48000.0 / rate);
    out.resize(out_frames * 2);

    for (size_t i = 0; i < out_frames; ++i) {
        double pos = (rate == 48000) ? static_cast<double>(i)
                                     : static_cast<double>(i) * rate / 48000.0;
        size_t i0 = static_cast<size_t>(pos);
        if (i0 >= in_frames) i0 = in_frames - 1;
        size_t i1 = (i0 + 1 < in_frames) ? i0 + 1 : i0;
        const double fr = pos - static_cast<double>(i0);

        for (int c = 0; c < 2; ++c) {
            const int sc = (channels == 2) ? c : 0;
            const double a = src[i0 * channels + sc];
            const double b = src[i1 * channels + sc];
            out[i * 2 + c] = static_cast<short>(a + (b - a) * fr);
        }
    }

    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "%u Hz, %u channel(s), %zu frames -> 48000 Hz stereo, %zu frames",
                  rate, channels, in_frames, out_frames);
    why = buf;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    fopen_s(&g_log, "dualsense_probe.log", "w");

    const char* wav = nullptr;
    unsigned limit_seconds = 15;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list") == 0) {
            Say("Cached game music decoded by the shell lives under:\n");
            Say("  src\\ui_csharp\\bin\\Release\\net9.0-windows\\win-x64\\Cache\\Audio\\*.wav\n");
            Say("Those are already 48 kHz stereo 16-bit, so they play directly.\n");
            if (g_log) std::fclose(g_log);
            return 0;
        }
        if (!wav) wav = argv[i];
        else limit_seconds = static_cast<unsigned>(std::atoi(argv[i]));
    }

    Say("DualSense AUDIO probe\n");
    GPU::DualSense::EnsureStarted();

    Sample s;
    bool connected = false;
    for (int i = 0; i < 50 && !connected; ++i) {
        connected = GPU::DualSense::GetSample(s) && s.connected;
        std::this_thread::sleep_for(100ms);
    }
    if (!connected) {
        Say("RESULT: no controller.\n");
        GPU::DualSense::Shutdown();
        if (g_log) std::fclose(g_log);
        return 1;
    }
    Say("Controller connected. transport=%s  battery=%u%%\n",
        GPU::DualSense::IsBluetooth() ? "Bluetooth" : "USB", s.battery_level);

    // Device information -- the new fields the Input tab will show. Printed on
    // every run so the INFERRED bit and offset assignments can be checked
    // against hardware: mute the mic, plug a cable, and see which bits move.
    Say("status: headphone=%d mic_jack=%d mic_muted=%d usb_data=%d usb_power=%d "
        "charging=%d full=%d\n",
        (int)s.headphone_connected, (int)s.mic_jack, (int)s.mic_muted,
        (int)s.usb_data, (int)s.usb_power,
        (int)s.battery_charging, (int)s.battery_full);
    {
        GPU::DualSense::FirmwareInfo fw;
        if (GPU::DualSense::ReadFirmwareInfo(fw)) {
            Say("firmware: main %u.%u.%u  sbl %u.%u.%u  dsp %04X_%04X  "
                "model rev 0x%04X  gen %u  built %s %s\n",
                (fw.main_version >> 24) & 0xFF, (fw.main_version >> 16) & 0xFF,
                fw.main_version & 0xFFFF,
                (fw.sbl_version >> 24) & 0xFF, (fw.sbl_version >> 16) & 0xFF,
                fw.sbl_version & 0xFFFF,
                (fw.dsp_version >> 16) & 0xFFFF, fw.dsp_version & 0xFFFF,
                fw.hardware_info & 0xFFFF, (fw.hardware_info >> 8) & 0xFF,
                fw.build_date, fw.build_time);
        } else {
            Say("firmware: feature report 0x20 unavailable\n");
        }
    }

    if (!GPU::DualSense::IsBluetooth()) {
        Say("\nOn USB the controller exposes real audio endpoints, so neither\n"
            "Bluetooth lane applies. Nothing to test here.\n");
        GPU::DualSense::Shutdown();
        if (g_log) std::fclose(g_log);
        return 0;
    }

    if (wav) {
        Banner("SPEAKER: playing a file");
        std::vector<short> pcm;
        std::string why;
        if (!LoadWav48kStereo(wav, pcm, why)) {
            Say("Could not load '%s': %s\n", wav, why.c_str());
            GPU::DualSense::Shutdown();
            if (g_log) std::fclose(g_log);
            return 1;
        }
        Say("Loaded '%s'\n  %s\n", wav, why.c_str());

        // Normalise, and say by how much.
        //
        // The first real-audio test was inaudible while a sine tone at a similar
        // nominal level was clear, and measurement explained it: that track
        // peaks near 30% of full scale, roughly 10 dB below the tone. The user
        // also heard only the 880 Hz tone and neither 440 nor 220, so this
        // speaker reproduces upper mids and little either side. Quiet broadband
        // music through a band-limited speaker falls below audibility even when
        // the transport is perfect.
        //
        // Boosting makes the comparison fair: the question under test is whether
        // the Opus path carries music, not whether one track happens to be
        // mastered quietly.
        {
            short peak = 1;
            for (short v : pcm) {
                const short a = static_cast<short>(v < 0 ? -v : v);
                if (a > peak) peak = a;
            }
            const double gain = 29000.0 / static_cast<double>(peak);
            Say("  peak %d of 32767 (%.0f%% FS), applying %.1fx gain\n",
                peak, peak * 100.0 / 32767.0, gain);
            if (gain > 1.0) {
                for (short& v : pcm) {
                    double x = v * gain;
                    if (x > 32767.0) x = 32767.0;
                    if (x < -32768.0) x = -32768.0;
                    v = static_cast<short>(x);
                }
            }
        }

        size_t frames = pcm.size() / 2;
        const size_t cap = static_cast<size_t>(limit_seconds) * 48000;
        if (frames > cap) {
            Say("  playing the first %u seconds of %.1f\n",
                limit_seconds, frames / 48000.0);
            frames = cap;
        }
        Say("Listen to the controller itself, not your PC speakers.\n");

        // A level sweep rather than one guess. Playback works but is far
        // quieter than the same pad on a PS5, and the two levers are speaker
        // volume (0-255) and the preamp gain. The first working version used
        // 0x40 volume -- a quarter scale -- simply because it was written before
        // anyone had heard it. Rather than guess again, play the same passage at
        // rising levels and let the ear decide.
        struct { unsigned char vol, preamp; const char* label; } levels[] = {
            { 0x50, 0x02, "1: volume 0x50, preamp 0x02  (the reference default)" },
            { 0xFF, 0x02, "2: volume 0xFF, preamp 0x02  (full volume)" },
            { 0xFF, 0x03, "3: volume 0xFF, preamp 0x03  (full volume, more preamp)" },
        };

        const size_t clip = frames < (8u * 48000u) ? frames : 8u * 48000u;
        bool ok = false;
        for (const auto& L : levels) {
            Say("\n--- %s ---\n", L.label);
            GPU::DualSense::SetBluetoothAudioLevels(L.vol, L.preamp);
            Countdown("Playing 8 s", 3);
            ok = GPU::DualSense::PlaySpeakerPcmBlocking(
                pcm.data(), clip, /*headset=*/false);
            Say("  returned %s\n", ok ? "TRUE" : "FALSE");
        }

        Banner("DONE");
        Say("Which of 1, 2 or 3 was loudest, and was any of them loud enough?\n");
        Say("If 3 beats 2, the preamp gain is worth pushing further.\n");
        Say("If 2 and 3 sound the same, volume alone is the control that counts.\n");
        GPU::DualSense::Shutdown();
        if (g_log) std::fclose(g_log);
        return ok ? 0 : 1;
    }

    Banner("SPEAKER: tones");
    Say("Listen to the controller itself, not your PC speakers.\n");
    struct { double hz; const char* label; } tones[] = {
        { 440.0, "A: 440 Hz" },
        { 880.0, "B: 880 Hz" },
        { 220.0, "C: 220 Hz" },
    };
    for (const auto& t : tones) {
        Say("\n--- %s ---\n", t.label);
        Countdown("Tone starts", 3);
        auto pcm = SpeakerTone(t.hz, 3, 12000.0);
        const bool ok = GPU::DualSense::PlaySpeakerPcmBlocking(
            pcm.data(), pcm.size() / 2, /*headset=*/false);
        Say("  returned %s\n", ok ? "TRUE" : "FALSE");
    }

    Banner("DONE");
    Say("Pass a .wav path to play real audio instead of tones.\n");
    GPU::DualSense::Shutdown();
    if (g_log) std::fclose(g_log);
    return 0;
}
