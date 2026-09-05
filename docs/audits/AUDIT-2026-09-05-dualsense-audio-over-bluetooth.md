# AUDIT — DualSense speaker and microphone over Bluetooth

**Date:** 2026-09-05
**Question:** the PS5 drives the DualSense speaker and microphone wirelessly.
Can PCSX5 do the same on Windows over Bluetooth?

## Classification

`HARD BOUNDARY` over Bluetooth on a stock Windows stack.
`SOFT BOUNDARY` over USB — see "What is actually achievable".

## Evidence

### 1. The device exposes no audio endpoints over Bluetooth (VERIFIED)

With the user's DualSense connected and working (HID input verified the same
day — full stick range, triggers, buttons, rumble, lightbar, mic LED, player
LEDs), Windows enumerates **no audio endpoint for it at all**:

```
Get-PnpDevice -PresentOnly | Where Class -in AudioEndpoint,MEDIA
```

Other Bluetooth devices on the same machine list their audio profiles plainly —
`Xiaomi 17 Ultra A2DP SNK`, `Xiaomi 17 Ultra Hands-Free HF Audio`,
`Smart TV Pro A2DP SNK`, `Noise Airwave Max 5 Hands-Free`. The controller
appears only under `HIDClass` and `Bluetooth`.

There is therefore no audio device for any host API — WASAPI, NAudio, XAudio2 —
to open. This is not a matter of picking the right API or the right driver.

### 2. Why the PS5 can and Windows cannot (INFERRED)

The DualSense speaks two layers over one Bluetooth link: standard HID, and a
proprietary Sony layer carrying haptics and audio as PCM inside custom packets.
The console implements that second layer. Windows implements standard Bluetooth
audio profiles (A2DP, HFP), which the controller does not advertise — hence the
empty endpoint list above.

The comparison in the user's question is exact and the answer is not "we are
worse at this": the PS5 is not doing Bluetooth audio in the standard sense at
all. It is speaking a protocol only Sony's radio stack speaks.

### 3. What the community has achieved (INFERRED)

The known way to obtain these features wirelessly on a PC is **replacement radio
hardware**, not host software: a Raspberry Pi Pico 2 W running `DS5Dongle`
firmware, or Sony's own adapter. Both work by being the thing that speaks the
proprietary protocol. No pure-software Windows solution is reported.

## What would be required to lift the boundary

One of:

1. A custom Bluetooth transport that bypasses the Windows stack and speaks
   Sony's proprietary audio layer directly. This needs the protocol, which is
   `UNKNOWN` here, and a driver — far outside an emulator's remit.
2. Dedicated dongle hardware, which is a purchase, not a code change.

Neither is emulator work. Recording the boundary is the correct outcome.

## What is actually achievable

Over **USB**, the DualSense enumerates as a USB Audio Class device and Windows
does expose speaker and microphone endpoints. `src/ui_csharp/DualSenseAudio.cs`
already targets that path via NAudio. Speaker output and mic capture are
therefore a normal, solvable task **when the controller is wired**.

**NEEDS_EVIDENCE:** this has not been confirmed on this machine. The test is to
connect the controller by USB cable and re-run the endpoint enumeration above,
looking for a "Wireless Controller" audio device.

## Consequence for the shell

Whatever the USB result, the shell must not present speaker and microphone
controls as though they work regardless of how the pad is attached. A control
that silently does nothing over Bluetooth is the same defect class as the rest
of this project's history. It should say why it is unavailable.

## Could a custom Windows driver fix it?

Asked directly, and it deserves a direct answer rather than a shrug. Split into
the two halves a driver would need.

### Half one: present a speaker and microphone to Windows — routine

A virtual audio driver (APO / AVStream, or a user-mode equivalent) that appears
in the endpoint list is ordinary, well-documented Windows work. This half is not
the problem.

### Half two: deliver the samples to the controller — this is where it fails

The driver would have somewhere to receive audio from and nowhere to send it.

**The HID channel cannot carry it.** Measured from the vendored library
(`IO.cpp`), the Bluetooth reports are **78 bytes**; USB is 64. Nearly all of
that is already spoken for — rumble, lightbar, player LEDs, trigger effects,
mic control, and the trailing CRC32. At the DualSense's Bluetooth report rate of
roughly 250 Hz, the entire report stream is about **20 KB/s**, and the free
space within it is a fraction of that.

Speaker audio needs far more. Even 16 kHz 16-bit mono — well below what the
console sends — is 32 KB/s. The channel is short by a factor of several before
a single byte is reserved for the data it already carries. This is not a tuning
problem; the pipe is the wrong size.

**So the real audio path is elsewhere.** It is a separate proprietary channel on
the same Bluetooth link, and its protocol is `UNKNOWN` to this project.

### What building it would actually require

1. Reverse-engineer Sony's proprietary Bluetooth audio layer. That means
   capturing traffic between a real PS5 and a DualSense with a Bluetooth
   sniffer — hardware this project does not have, against a protocol nobody has
   published.
2. Write a **kernel-mode Bluetooth transport/profile driver** to speak it,
   because the Windows stack will not open a channel it has no profile for.
3. Get it signed. Unsigned kernel drivers need test-signing or dev mode on every
   machine that runs PCSX5.

That is a standalone systems project of considerable size, with a research phase
that might simply fail, and it is not emulator work. The strongest evidence
against attempting it is that the community did not: the working solutions are
replacement radio hardware (a Pico 2 W running DS5Dongle, or Sony's adapter) —
people chose to build *hardware that speaks the protocol* rather than a Windows
driver. That is what you do when the software route is blocked, not when it is
merely tedious.

### Recommendation

Do not build a driver. Implement speaker and microphone over **USB**, where
Windows already exposes the endpoints and `DualSenseAudio.cs` already aims at
them, and make the shell state plainly that these features need a cable. An
honest "unavailable over Bluetooth, connect by USB" is worth more to a user than
a control that silently does nothing — which is precisely the defect class this
project has spent its effort removing.
