// DualSense audio / haptics probe -- manual, hardware-only.
//
// Trimmed to the audio path alone. Sticks, triggers, buttons, touch, lightbar,
// rumble, mic LED and player LEDs were all verified working on real hardware on
// 2026-09-05 and are recorded as such in TASKS.md; re-testing them every run
// only buries the one thing still unresolved.
//
// Deliberately NOT a CTest: it needs a controller and a person to say whether
// they felt anything.
//
//     build\Release\dualsense_probe.exe
//
// Writes dualsense_probe.log beside the working directory.
#include "gpu/dualsense_hid.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
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

void Countdown(const char* what, int seconds) {
    Say("%s in ", what);
    for (int i = seconds; i > 0; --i) {
        Say("%d... ", i);
        std::this_thread::sleep_for(1s);
    }
    Say("NOW\n");
}

// SIGNED 8-bit PCM, 3000 Hz, stereo interleaved. Silence is 0, not 128 --
// fixes, not one we chose.
std::vector<unsigned char> Tone(double hz, unsigned seconds, double amplitude) {
    const unsigned rate = 3000;
    std::vector<unsigned char> pcm(rate * 2 * seconds);
    for (unsigned i = 0; i < rate * seconds; ++i) {
        const double t = static_cast<double>(i) / rate;
        const double v = std::sin(2.0 * 3.14159265358979 * hz * t);
        const unsigned char b = static_cast<unsigned char>(
            static_cast<signed char>(amplitude * v));
        pcm[i * 2 + 0] = b;
        pcm[i * 2 + 1] = b;
    }
    return pcm;
}

bool Attempt(const char* label, double hz, unsigned seconds, double amplitude) {
    Say("\n--- %s ---\n", label);
    Say("Hold the controller firmly in both hands.\n");
    Countdown("Starting", 4);
    auto pcm = Tone(hz, seconds, amplitude);
    Say("  streaming %zu bytes: %.0f Hz, %u s\n", pcm.size(), hz, seconds);
    const bool ok = GPU::DualSense::PlayHapticsPcmBlocking(pcm.data(), pcm.size());
    Say("  returned %s\n", ok ? "TRUE" : "FALSE");
    return ok;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    fopen_s(&g_log, "dualsense_probe.log", "w");

    Say("DualSense AUDIO probe (haptics over Bluetooth)\n");
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

    if (!GPU::DualSense::IsBluetooth()) {
        Say("\nOn USB the controller exposes real audio endpoints, so the\n"
            "Bluetooth haptics report does not apply. Nothing to test here.\n");
        GPU::DualSense::Shutdown();
        if (g_log) std::fclose(g_log);
        return 0;
    }

    Say("\nThe log line naming OutputReportByteLength is the diagnostic that\n"
        "matters most -- it says what size write the device will accept.\n");

    // Three attempts, varying the two things most likely to make a real effect
    // imperceptible rather than absent: frequency and amplitude.
    Attempt("A: 60 Hz, moderate", 60.0, 3, 100.0);
    Attempt("B: 150 Hz, full scale", 150.0, 3, 127.0);
    Attempt("C: 30 Hz, full scale", 30.0, 3, 127.0);

    Say("\n========== DONE ==========\n");
    Say("Did ANY of A, B or C produce a buzz or vibration? Which?\n");
    GPU::DualSense::Shutdown();
    if (g_log) std::fclose(g_log);
    return 0;
}
