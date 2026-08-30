# ADR-001: The core owns controller input

- Status: Accepted
- Date: 2026-08-30
- Supersedes: None
- Superseded By: None

## Context

DualSense support was implemented twice, independently:

- the emulator core read the controller over raw HID for the guest
  (`src/hle/libscepad.cpp`, `src/gpu/input/`);
- the WPF shell read it again in C# (`src/ui_csharp/WindowsDualSenseReader.cs`)
  purely to drive the Controller Setup screen.

Neither implementation could serve the other, and the shell had no way to tell
the core anything about input at all.

## Current Evidence

Two concrete blockers, established by inspection rather than assumed:

1. **There is no input entry point in the native ABI.** `CoreBridge.cs` declares
   exactly `pcsx5_init`, `pcsx5_load`, `pcsx5_run`, `pcsx5_stop`,
   `pcsx5_force_stop`, `pcsx5_pause`, `pcsx5_resume`, `pcsx5_shutdown`,
   `pcsx5_extract_pkg` and `pcsx5_get_last_error`. Nothing accepts input state,
   so the shell cannot inject a pad state for any player.

2. **The kernel accepts exactly one pad.** `src/hle/libpad.cpp` rejects
   `scePadOpen` unless the request is for the primary user and index zero:

   ```c
   if (userId != SCE_PAD_PRIMARY_USER_ID || !type_accepted || index != 0 || ...)
   ```

   A second controller is refused at open time, so multi-player input is not
   partially working — it is unreachable.

## Decision

**The core owns device input.** The vendored DualSenseWindows library
(`third_party/DualSenseWindows`) drives a single reader in the core; the shell
reads pad state through a new, versioned ABI entry point rather than opening the
device itself.

## Why

The core already performs HID work and already needs pad state for the guest.
Keeping one owner means the guest and the configuration UI see the same device,
the core stays headless-testable, and the duplicate C# reader can be retired.

The alternative — leaving the shell's reader in place — would keep two
independent DualSense implementations in the tree and would still leave the guest
blind to the sticks.

## Alternatives Considered

- **Shell owns input, injecting state into the core.** Rejected: it moves HID
  handling away from the layer that needs it for the guest, and makes headless
  runs depend on a UI process.
- **Core reads a per-player device assignment from configuration.** Avoids an ABI
  change, but gives the shell no way to show live pad state in Controller Setup.

## Consequences

- A public ABI addition (for example `pcsx5_get_pad_state(user_index, state)`).
  Shell and core ship separately, so a signature mismatch fails at runtime rather
  than at compile time; the entry point must be versioned and documented.
- `src/ui_csharp/WindowsDualSenseReader.cs` is retired once the shell reads
  through the ABI.
- The single-pad restriction in `libpad.cpp` must be relaxed, with handle
  lifetime and per-user state tracked correctly, before two controllers work.

## Compatibility Impact

Adds to the native ABI; does not change existing exported signatures.

## Runtime Impact

One reader thread in the core instead of up to three independent readers. Guest
titles gain working analog sticks, rumble and adaptive triggers.

## Testing Impact

Pad plumbing becomes reachable from headless runs. Multi-controller behaviour
needs a test that opens a second user and exercises teardown.

## Migration

1. Reimplement the DualSense reader in the core on DualSenseWindows — **done**;
   see `src/gpu/dualsense_ds5w.cpp`.
2. Add the pad-state ABI entry point.
3. Point the shell at it and delete `WindowsDualSenseReader.cs`.
4. Relax the `scePadOpen` restriction and track per-user state.

## Rollback / Containment

Steps are independent. The core-side reader stands alone; if the ABI addition is
reverted the shell simply keeps its own reader until it is retried.

## Related ADRs

None.
