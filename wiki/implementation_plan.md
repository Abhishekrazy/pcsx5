# Boot Gaps + Release Build Script — Implementation Plan

## Overview

Two parallel tracks:
1. **Release Build Script** — `dist/build_release.ps1` targeting `i:\Personal\Windows\pcsx5\dist`
2. **Boot Gap Fixes** — fill every mandatory HLE module and harden Lua init so the emulator can boot fake-signed PS5 games

---

## Track 1 — Release Build Script

### [NEW] dist/build_release.ps1

A clean script at `dist/build_release.ps1` that:

1. Builds C++ core (`pcsx5_core.dll` + `pcsx5_cli.exe`) via **CMake Ninja Release**
2. Publishes WPF frontend (`pcsx5.exe`) via **dotnet publish** (single-file, self-contained)
3. Stages all outputs + assets into `dist/`
4. Validates mandatory files are present before finishing
5. Optional `-Zip` flag to create `PCSX5_YYYYMMDD.zip`
6. Optional `-Clean` flag to wipe `build/` and `dist/` first

> [!IMPORTANT]
> **Build command** (run from repo root `i:\Personal\Windows\pcsx5`):
> ```powershell
> .\dist\build_release.ps1           # standard release build
> .\dist\build_release.ps1 -Clean    # clean build
> .\dist\build_release.ps1 -Zip      # build + zip
> ```

---

## Track 2 — Mandatory Boot Gap Fixes

The following are all required for a game to proceed past `pcsx5_init()` → ELF load → `main()`.

---

### Fix 1 — Lua Init Hardening

#### [MODIFY] [lua_init.cpp](file:///i:/Personal/Windows/pcsx5/src/lua/lua_init.cpp)
- Embed `default_init.lua` as a C string literal compiled into the binary
- `RunDefaultInit()` falls back: disk `assets/pcsx5_init.lua` → embedded string → C++ fallback chain
- **Boot never fails** due to a missing `.lua` file on disk

---

### Fix 2 — Six New HLE Modules (new .cpp files in src/hle/)

All 6 are called by the PS5 SDK during module initialization, before `main()` is reached.

#### [NEW] src/hle/libappcontent.cpp — libSceAppContent
- `sceAppContentInitParam` → no-op, return 0  
- `sceAppContentGetAddcontInfoList` → empty list (0 items)
- `sceAppContentGetEntitlementKey` → 32 zero bytes, return 0
- `sceAppContentSmallSize` → return 1 (content present)
- `sceAppContentGetPftFlag` → 0
- `sceAppContentAddcontMount` → return 0

#### [NEW] src/hle/librtc.cpp — libSceRtc
- `sceRtcGetCurrentTick` → fills `SceRtcTick` from `GetSystemTimeAsFileTime()`
- `sceRtcGetCurrentClockLocalTime` / `sceRtcGetCurrentClockUtc` → fills `SceRtcDateTime`
- `sceRtcTickAddSeconds` / `sceRtcTickAddMinutes` / `sceRtcTickAddDays` → tick arithmetic
- `sceRtcConvertLocalTimeToUtc` → identity
- `sceRtcCheckValid` → always return 0 (valid)

#### [NEW] src/hle/libfiber.cpp — libSceFiber (via Windows Fibers)
- `sceFiberInitialize` → `CreateFiber()`, store context
- `sceFiberRun` → `SwitchToFiber()` on first run
- `sceFiberSwitch` → fiber context switch
- `sceFiberReturnToThread` → return to thread fiber
- `sceFiberFinalize` → `DeleteFiber()`
- `sceFiberGetSelf` → return current fiber pointer

#### [NEW] src/hle/libregmgr.cpp — libSceRegMgr
- `sceRegMgrGetInt` → write 0 to output, return 0
- `sceRegMgrGetStr` → write empty string, return 0
- `sceRegMgrGetBin` → zeroed buffer, return 0
- `sceRegMgrSetInt` / `sceRegMgrSetStr` → no-op, return 0

#### [NEW] src/hle/libnotification.cpp — libSceNotification / libSceNpNotification
- `sceNotificationUtilInitialize` → return 0
- `sceNotificationUtilSendNotification` → log the message, return 0
- `sceNotificationUtilCleanup` → no-op, return 0
- `sceNpNotificationRequestSendNotification` → return 0

#### [NEW] src/hle/libnet.cpp — libSceNet + libSceNetCtl
- `sceNetInit` / `sceNetTerm` → return 0
- `sceNetCtlInit` / `sceNetCtlTerm` → return 0
- `sceNetCtlGetState` → write state=3 ("connected") so games don't spin-retry
- `sceNetCtlCheckCallback` → no-op, return 0
- `sceNetSocket` → return dummy fd (e.g. 100), `sceNetClose` → return 0
- `sceNetConnect` / `sceNetBind` → return error (offline stub)

---

### Fix 3 — Sysmodule ID Map Expansion

#### [MODIFY] [libsysmodule.cpp](file:///i:/Personal/Windows/pcsx5/src/hle/libsysmodule.cpp)

Expand `KnownSysmoduleNames()` to cover all IDs commonly loaded at boot:
```
0x0001 libSceAppContent    0x0002 libSceNpManager
0x0003 libSceNpTrophy      0x0004 libSceAudioOut
0x0005 libSceVideoOut      0x0006 libSceFiber
0x0007 libSceNet           0x0008 libSceNetCtl
0x0009 libSceHttp          0x000A libSceSsl
0x000B libSceRtc           0x000C libSceRegMgr
0x000D libSceSystemService 0x0021 libSceRudp
0x005A libSceAvSetting     0x0065 libSceLibcInternal
0x0095 libSceIme           0x0096 libSceImeDialog
0x00A9 libSceMouse         0x00B0 libSceNotification
0x00F0 libSceCommonDialog  0x0120 libSceGnm
0x0130 libSceGnmDriver
```

---

### Fix 4 — Register New Modules in HLE Init

#### [MODIFY] [hle.cpp](file:///i:/Personal/Windows/pcsx5/src/hle/hle.cpp)

Add forward declarations and `Initialize()` calls for all 6 new modules (before `RegisterNidDbStubs()`).

---

### Fix 5 — dist/ Asset Deployment

The `dist/` directory must contain `assets/` with mandatory runtime files.

#### [MODIFY] dist/build_release.ps1

Must copy these to `dist/assets/`:
- `src/lua/default_init.lua` → `dist/assets/pcsx5_init.lua`
- `assets/nid_db.txt` → `dist/assets/nid_db.txt`
- `assets/lang/` → `dist/lang/`
- `assets/config.ini` → `dist/config.ini`
- `pcsx5_config/global.json` → `dist/pcsx5_config/global.json`

---

## Verification Plan

### Automated Tests
```powershell
# After build_release.ps1 completes:
.\dist\pcsx5_cli.exe `
    --config-dir=.\dist\pcsx5_config `
    --title-id=PCSX5TEST `
    .\tests\test_elf\test_guest.elf
# Expected: exit 0, "Hello Guest" in output

# With a real fake-signed game:
.\dist\pcsx5_cli.exe `
    --config-dir=.\dist\pcsx5_config `
    --title-id=CUSA00001 `
    --log-file=boot.log `
    D:\Games\FakeSignedGame\eboot.bin
# Look for: "Starting guest execution" in boot.log
# Look for NO: "UNIMPLEMENTED" for any of the 6 new modules
```

### Manual Verification
- Launch `dist/pcsx5.exe`, add game folder with controller
- Boot a fake-signed game and verify `GameBootOverlay` progresses to "Starting"
- Confirm pause menu (PS button) works during a running game
