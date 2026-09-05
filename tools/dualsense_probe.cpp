// Manual DualSense hardware probe.
//
// Deliberately NOT registered with CTest. It needs a physical controller and a
// person to confirm what they felt and saw, so as an automated test it could
// only ever pass vacuously on a machine with no pad attached -- the exact kind
// of instrument this project has spent its time removing.
//
// Run it from a terminal you can watch, because the prompts are the point:
//     build\Release\dualsense_probe.exe
//
// Everything printed is also written to dualsense_probe.log next to the working
// directory, so the run can be read back afterwards. Capturing the console
// instead would hide the live prompts from the person holding the controller.
#include "gpu/dualsense_hid.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <thread>

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

// Give the person time to look at the controller before anything happens.
void Countdown(const char* what, int seconds) {
    Say("%s in ", what);
    for (int i = seconds; i > 0; --i) {
        Say("%d... ", i);
        std::this_thread::sleep_for(1s);
    }
    Say("NOW\n");
}

bool WaitForController(int seconds) {
    Sample s;
    for (int i = 0; i < seconds * 10; ++i) {
        if (GPU::DualSense::GetSample(s) && s.connected) return true;
        std::this_thread::sleep_for(100ms);
    }
    return false;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    fopen_s(&g_log, "dualsense_probe.log", "w");

    Say("DualSense probe: starting reader...\n");
    GPU::DualSense::EnsureStarted();

    if (!WaitForController(5)) {
        Say("RESULT: no DualSense report stream after 5s.\n");
        GPU::DualSense::Shutdown();
        if (g_log) std::fclose(g_log);
        return 1;
    }
    Say("RESULT: controller connected.\n");

    Sample s;
    GPU::DualSense::GetSample(s);
    Say("battery=%u%% charging=%d full=%d headphones=%d\n",
        s.battery_level, (int)s.battery_charging,
        (int)s.battery_full, (int)s.headphone_connected);

    Banner("1 of 5: STICKS AND TRIGGERS");
    Say("Put both thumbs on the sticks.\n");
    Say("When it says NOW: sweep BOTH sticks in full circles, squeeze BOTH\n");
    Say("triggers all the way, and press some face buttons. 10 seconds.\n");
    Countdown("Starting", 5);

    u8 lx_min = 255, lx_max = 0, ly_min = 255, ly_max = 0;
    u8 rx_min = 255, rx_max = 0, ry_min = 255, ry_max = 0;
    u8 l2_max = 0, r2_max = 0, touch_max = 0;
    u32 buttons_seen = 0;
    int samples = 0;
    for (int i = 0; i < 100; ++i) {
        if (GPU::DualSense::GetSample(s) && s.connected) {
            ++samples;
            if (s.lx < lx_min) lx_min = s.lx;  if (s.lx > lx_max) lx_max = s.lx;
            if (s.ly < ly_min) ly_min = s.ly;  if (s.ly > ly_max) ly_max = s.ly;
            if (s.rx < rx_min) rx_min = s.rx;  if (s.rx > rx_max) rx_max = s.rx;
            if (s.ry < ry_min) ry_min = s.ry;  if (s.ry > ry_max) ry_max = s.ry;
            if (s.l2 > l2_max) l2_max = s.l2;
            if (s.r2 > r2_max) r2_max = s.r2;
            if (s.touch_count > touch_max) touch_max = s.touch_count;
            buttons_seen |= s.buttons;
        }
        if (i % 20 == 19) Say("  ...%d seconds left\n", (100 - i) / 10);
        std::this_thread::sleep_for(100ms);
    }
    Say("\nsamples=%d\n", samples);
    Say("left  stick x:[%u..%u] y:[%u..%u]\n", lx_min, lx_max, ly_min, ly_max);
    Say("right stick x:[%u..%u] y:[%u..%u]\n", rx_min, rx_max, ry_min, ry_max);
    Say("triggers  L2 max=%u  R2 max=%u\n", l2_max, r2_max);
    Say("buttons bitmask=0x%08X   touch fingers max=%u\n", buttons_seen, touch_max);
    Say("VERDICT sticks:   %s\n",
        (lx_max - lx_min > 60 && ly_max - ly_min > 60 &&
         rx_max - rx_min > 60 && ry_max - ry_min > 60)
            ? "OK, both sticks sweep" : "BROKEN, at least one axis did not move");
    Say("VERDICT triggers: %s\n",
        (l2_max > 60 && r2_max > 60) ? "OK, both triggers read"
                                     : "BROKEN, at least one trigger did not read");
    Say("VERDICT buttons:  %s\n",
        buttons_seen ? "OK, buttons register" : "BROKEN, no button registered");

    Banner("2 of 5: LIGHTBAR");
    Say("Look at the light strip either side of the touchpad.\n");
    Countdown("Colour cycle starts", 5);
    const u8 colours[3][3] = { {255,0,0}, {0,255,0}, {0,0,255} };
    const char* names[3] = { "RED", "GREEN", "BLUE" };
    for (int c = 0; c < 3; ++c) {
        Say("  lightbar -> %s\n", names[c]);
        GPU::DualSense::SetLightBar(colours[c][0], colours[c][1], colours[c][2]);
        std::this_thread::sleep_for(3s);
    }
    GPU::DualSense::SetLightBar(0, 0, 0);

    Banner("3 of 5: RUMBLE");
    Say("Hold the controller in both hands.\n");
    Countdown("Motors start", 5);
    Say("  HEAVY motor (left side)\n");
    GPU::DualSense::SetRumble(200, 0);
    std::this_thread::sleep_for(2500ms);
    GPU::DualSense::SetRumble(0, 0);
    std::this_thread::sleep_for(1s);
    Say("  LIGHT motor (right side)\n");
    GPU::DualSense::SetRumble(0, 200);
    std::this_thread::sleep_for(2500ms);
    GPU::DualSense::SetRumble(0, 0);

    Banner("4 of 5: MICROPHONE MUTE LED");
    Say("Look at the small button just below the touchpad.\n");
    Countdown("Mic LED lights", 5);
    Say("  mic LED ON for 4 seconds\n");
    GPU::DualSense::SetMicLed(1);
    std::this_thread::sleep_for(4s);
    GPU::DualSense::SetMicLed(0);

    Banner("5 of 5: PLAYER LEDs");
    Say("Look at the row of small LEDs under the touchpad.\n");
    Countdown("Player LEDs light", 5);
    Say("  centre LED only\n");
    GPU::DualSense::SetPlayerLeds(0x04, false);
    std::this_thread::sleep_for(3s);
    Say("  all five\n");
    GPU::DualSense::SetPlayerLeds(0x1F, false);
    std::this_thread::sleep_for(3s);
    GPU::DualSense::SetPlayerLeds(0x00, false);

    Banner("DONE");
    Say("Tell me which of these four actually happened:\n");
    Say("  lightbar colours / rumble motors / mic LED / player LEDs\n");
    GPU::DualSense::Shutdown();
    if (g_log) std::fclose(g_log);
    return 0;
}
