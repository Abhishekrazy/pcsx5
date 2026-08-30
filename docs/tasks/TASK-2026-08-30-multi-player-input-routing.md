# TASK-2026-08-30-multi-player-input-routing

Support assigning an input device per player — for example Player 1 on a
DualSense and Player 2 on the keyboard — and let more than one controller be
used at once.

## Status

**Blocked on core work.** Specified here rather than started, because the shell
cannot deliver it alone and a settings control that does nothing would be worse
than none.

## Why the shell cannot do this

Two blockers, both established by inspection rather than assumed:

### 1. There is no input entry point in the native ABI

`src/ui_csharp/CoreBridge.cs` exposes exactly:

```
pcsx5_init   pcsx5_load   pcsx5_run    pcsx5_stop   pcsx5_force_stop
pcsx5_pause  pcsx5_resume pcsx5_shutdown
pcsx5_extract_pkg          pcsx5_get_last_error
```

Nothing accepts input state. The core reads controllers itself; the shell has no
way to inject a pad state, for any player, from any device. So "Player 2 uses the
keyboard" cannot be expressed across the boundary as it stands.

### 2. The core accepts exactly one pad

`src/hle/libpad.cpp` rejects `scePadOpen` unless the request is for the primary
user and index zero:

```c
if (userId != SCE_PAD_PRIMARY_USER_ID || !type_accepted || index != 0 || ...)
```

A second controller is refused at open time, so multi-player input is not
partially working — it is not reachable.

## What a solution needs

1. **Decide the boundary.** Either
   - add a versioned input entry point to the native ABI (for example
     `pcsx5_set_pad_state(user_index, state)`), letting the shell own device
     enumeration, keyboard synthesis and per-player assignment; or
   - keep input ownership in the core and have it read a per-player device
     assignment from configuration.

   The first keeps device handling in the layer that already does HID work and
   keeps the core headless-testable. The second avoids an ABI change. This is an
   architecture decision and a public-ABI change, so it is a stopping condition
   under Rule 10 — it needs explicit approval before implementation.

2. **Relax the single-pad restriction** in `libpad.cpp` / `libscepad.cpp` so
   `scePadOpen` accepts additional users and indices, with handle lifetime and
   per-user state tracked correctly.

3. **Synthesize analog values from the keyboard.** The shell can already map keys
   to stick-direction slots (done, see
   `docs/walkthroughs/WALKTHROUGH-2026-08-30-ui-05-playstation-pass.md`), but
   turning "W held" into an analog axis value delivered to a specific guest user
   is runtime work on whichever side owns input after step 1.

4. **Per-player device assignment UI**, once there is something behind it.

## Acceptance

- Two controllers can be used simultaneously and are seen by the guest as
  distinct users.
- A player can be assigned to the keyboard and control the guest with it.
- The assignment persists across restarts.
- `scePadOpen` for a second user succeeds and its handle behaves correctly
  through to teardown.
- No regression in `tests/runtime_baseline.json`.

## Related

The DualSense feature work (motor, lightbar, mute, stick reporting, diagram
overlay alignment, live test panel) is tracked separately and shares the
reference implementation the user selected,
<https://github.com/Ohjurot/DualSense-Windows>.

## Already delivered in the shell

- Automatic active-device detection with prompts that follow the device.
- Keyboard binding assignment, including stick-direction slots.
- Full keyboard navigation of the shell.
