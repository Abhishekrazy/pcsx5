# AUDIT-2026-08-30-dualsense-input-and-audio

Findings from replacing PCSX5's DualSense handling with the vendored
DualSenseWindows library, and from investigating the reported speaker/microphone
failures.

## Boundary ledger

| Boundary | Subject | Evidence | Classification | Result |
|---|---|---|---|---|
| Per-TU state duplication | `src/gpu/dualsense_hid.h` | 30 `static` namespace-scope globals in a header; 3 including TUs | **VERIFIED** | Fixed — state moved to one TU |
| Report-offset guessing | `src/gpu/dualsense_hid.h` | runtime layout heuristic (`g_last_layout`, `g_layout_streak`, `g_b0_pos`) | **VERIFIED** | Fixed — DS5W knows the layouts |
| Thread teardown crash | `dualsense_ds5w.cpp` | `hle_phase3` exit `0xC0000409` | **VERIFIED** | Fixed — leaked singleton |
| Single-pad restriction | `src/hle/libpad.cpp` | `userId != SCE_PAD_PRIMARY_USER_ID \|\| index != 0` | **VERIFIED** | Open — task specified |
| DualSense audio over Bluetooth | Windows BT stack | no audio profile advertised | **HARD BOUNDARY** | Not implementable |

## 1. Controller output did nothing — root cause

`src/gpu/dualsense_hid.h` carried the entire implementation inline, including
**30 `static` namespace-scope variables**: the device handle, the output state,
the sample buffer, the mutexes and the reader thread.

`static` at namespace scope has **internal linkage**. Every translation unit that
included the header therefore received its *own private copy* of all of it. The
three consumers — `src/hle/libscepad.cpp`, `src/gpu/vulkan_backend.cpp` and
`src/gpu/input/dualsense_input_backend.cpp` — were operating three unrelated
device states.

Consequence: a `SetRumble`, `SetLightBar` or `SetMicLed` call from one TU wrote
to a handle that a *different* TU had opened, so it silently did nothing. Up to
three reader threads could also contend for the same HID device.

This is sufficient on its own to explain the reported dead rumble, lightbar and
microphone LED.

## 2. Sticks reading as centred — second root cause

The same reader derived HID report offsets **at runtime**, scoring candidate
layouts with a heuristic (`g_last_layout`, `g_layout_streak`, `g_b0_pos`). When
the guess was wrong, stick bytes were read from the wrong offsets and reported as
centred.

DualSenseWindows implements the documented USB and Bluetooth layouts directly,
including the **CRC32 that the DualSense requires on Bluetooth output reports** —
another likely contributor to output failing specifically over Bluetooth, which
is how the test device is connected.

## 3. A crash introduced and fixed during the change

The first DS5W implementation held the reader as a namespace-scope
`std::thread`. Any process that uses the core but never calls `Shutdown()` — every
test binary — runs that destructor at static teardown while the thread is still
joinable, and `~thread()` on a joinable thread calls `std::terminate()`.

Observed as `hle_phase3` failing with exit code `0xC0000409` (`__fastfail`).

Fixed by moving all reader state into a deliberately leaked singleton and holding
the thread by pointer, so nothing is destroyed at exit. A bare `detach()` would
not have been sufficient: the thread could still touch a destroyed mutex while
the process tore down.

Note on measurement: an earlier CTest run reported four failures
(`self_header`, `reports`, and two others). That was an artifact of running CTest
while a full build was still writing binaries, not a real result. The reproducible
failure was `hle_phase3` alone.

## 4. Speaker and microphone — USB only

**Classification: HARD BOUNDARY over Bluetooth.**

The DualSense exposes its speaker and microphone as a **USB Audio Class** device,
not over HID. PCSX5 handles them shell-side through NAudio/WASAPI
(`src/ui_csharp/DualSenseAudio.cs`), which is the correct approach — the vendored
DS5W library is HID-only and exposes the microphone *LED* but no audio.

The test controller is connected over Bluetooth:

```
DeviceID : BTHENUM\DEV_7C66EF88642E\...BLUETOOTHDEVICE_7C66EF88642E
```

Enumerating every PnP node under that Bluetooth address gives exactly three:

| Class | Name |
|---|---|
| Bluetooth | DualSense Wireless Controller |
| HIDClass | Bluetooth HID Device |
| Bluetooth | Device Identification Service |

There is **no `MEDIA` class node, no `A2DP SNK`, and no Hands-Free/Headset
service**. For contrast, other devices paired to the same machine do expose them
(`MEDIA … A2DP SNK`, `System … Hands-Free HF`), so their absence here is a
property of the controller, not of the host.

Correspondingly, no DualSense audio endpoint appears among the active WASAPI
render or capture endpoints.

**Conclusion:** the DualSense advertises no Bluetooth audio profile, so there is
nothing for a host audio stack to connect to. Speaker and microphone audio cannot
be implemented over Bluetooth on the standard Windows stack. The PS5 itself uses
a proprietary link that is not exposed to PCs.

What would change this classification: evidence that the controller accepts audio
over an undocumented Bluetooth channel, which would require reverse-engineering
Sony's proprietary protocol. Until such evidence exists this remains
`UNKNOWN`-by-design rather than a defect to chase.

**Action taken:** none in code. The controller test panel should present speaker
and microphone tests gated on transport, showing "USB only — currently connected
over Bluetooth" rather than an inert button.

## 5. Verification

```bash
cmake --build build --config Release            # 0 errors
ctest --test-dir build -C Release --output-on-failure   # 46/46
```

The public API and `Sample` layout were preserved exactly, including the SCE_PAD
button bit encoding, so `libscepad`, `vulkan_backend` and the input backend
required no changes.

**Not yet verified against hardware:** input, rumble, lightbar and trigger
behaviour through the new backend have not been exercised with a physical
controller in this environment. The build and lifetime correctness are confirmed;
the device behaviour is not.

## 6. Open

- `libpad.cpp` still accepts a single pad — see
  `docs/tasks/TASK-2026-08-30-multi-player-input-routing.md`.
- The shell still has its own C# DualSense reader
  (`src/ui_csharp/WindowsDualSenseReader.cs`), which is now a parallel
  implementation. It should be replaced by reads through a core ABI entry point;
  that ABI addition is the approved Option A direction and is pending.
