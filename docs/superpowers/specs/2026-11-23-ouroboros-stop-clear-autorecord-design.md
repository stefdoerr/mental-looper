# Design: Ouroboros round 1 — stop/restart, clear all, auto-record

Builds on `2026-11-20-ouroboros-looper-design.md`. Everything not named here stays as it is.

## Why

The user ends songs and starts new ones on the Dwarf without touching the web UI. Today the only way
to silence the loops is to mute or clear each track, and there is no way to start the same loops
again from the top. Auto-record lets them press B and then get their hands on the guitar, so the
loop starts on the first note instead of on the press.

## Goals

- **Stop everything** with a double-tap on footswitch C. **Restart** from the top with B.
- **Clear everything** by holding C while stopped.
- **Auto-record** as an option: B arms an empty track, and recording starts when the input crosses a
  threshold.
- No clicks anywhere: not at the stop, not at the restart, and none baked into the loop.

## Non-goals

- Longer tracks (round 2) and parts/sequencer (round 3).
- Pause/resume in place. A stop always comes back from the top.
- Changing the undo engine (see Approaches).

## Approaches

The engine applies an undo, a redo, or the silencing of a reused track buffer as the playhead passes,
over one loop pass. A restart jumps the playhead to the top, so such a change can be half applied when
it happens.

1. **Restart-safe changes (picked by the user).** A half-applied change records the stretch of the loop
   it still owes. After the restart it applies that stretch when the playhead reaches it. Restart is
   instant. SooperLooper handles its lazy loop copies the same way (position marks, `fillLoops` in
   `src/plugin.cc`).
2. **Finish while stopped.** The engine finishes pending changes faster than real time while stopped,
   and a restart waits for that. Less code, but restart can lag up to ~0.5 s on a 5 min loop.
3. **Mix the layers live** instead of baking them into the track. Undo becomes a fade, and nothing is
   ever half applied. But CPU then grows with every layer, and it means rewriting the engine's core.
   Rejected for now by the user, who chose to keep the current engine. It stays the fallback if undo
   misbehaves on the hardware.

## Behaviour

### Footswitch C gestures

| Gesture | Running | Stopped |
|---|---|---|
| Tap | Undo, applied when the **double-tap window** runs out without a second press | Undo (same delay) |
| Double-tap (2nd press inside the window) | **Stop**, on the second press-down | — |
| Hold ≥ Hold Time | Redo (unchanged) | — |
| Hold ≥ 2 × Hold Time | — | **Clear all** (resets L) |

- The double-tap window is a setting, default 300 ms. The tap delay is the price of the double-tap,
  and the user accepted it.
- *Claude's call:* a press counts toward clear-all only if it started while already stopped. A
  double-tap whose second press is held stops, and nothing more happens.
- *Claude's call:* while stopped, a hold between 1× and 2× Hold Time does nothing, so redo is not
  available while stopped.

### Stop

- *Claude's call:* the stop first cancels an arm. Then any recording or overdub is closed exactly as a
  B press would close it, to Playing and never Rec→Dub. If that leaves every track empty (a first
  recording shorter than the minimum loop is discarded), there is nothing to stop.
- **Stop mode** setting (decided by the user):
  - **Instant** (default): the output fades out over 15 ms.
  - **End of loop**: everything plays to the end of the pass and stops exactly at the top. The output
    fades over the last 15 ms before the top, so the playhead never jumps.
  - **Fade**: the output fades out over **Fade time** (default 3 s), then stops. *Claude's call:* use
    an even-sounding curve, not linear amplitude.
- *Claude's call:* a tail or punch-out fade that is still writing input runs to its end before the
  engine parks, at most the Tail setting. The output is already silent by then. The loop ends up
  exactly as if there had been no stop.
- Parked: the playhead is at the top and the loops are intact. The output is the dry signal (if Dry
  thru is on) or silence. Input history keeps recording.
- *Claude's call:* a second double-tap during a pending End-of-loop or Fade stops at once (Instant).

### Restart (B while stopped)

- Every non-empty track plays from the top, keeping its on/off and volume. The selected track's state
  does not matter, and B only restarts. The next B press works as usual.
- *Claude's call:* the restart fades in over ~3 ms, like the recording fade-in, so the downbeat keeps
  its attack.
- *Claude's call:* **B during a pending End-of-loop or Fade cancels the stop.** Playing simply
  continues, and the output comes back up over 15 ms. B during Instant's short finishing window (≤ the
  Tail setting) restarts from the top as soon as the engine has parked.

### While stopped

- Tap-undo works. The change is applied from the top when playback restarts.
- Track select, on/off, volume, Clear track and Clear all work as always.
- If undo or clear leaves every track empty, L resets as it does today, and the looper is no longer
  stopped.

### Clear all

- Holding C past 2× Hold Time while stopped clears everything and resets L, as the existing Clear all
  trigger does. The looper then leaves the stopped state, and the next B records, or arms if
  auto-record is on.

### Auto-record

- Settings: **Auto Rec** (off by default) and **Threshold** (−60…−10 dBFS, default −40).
- With Auto Rec on, B on an empty track **arms** it instead of recording. This covers both the first
  loop and an empty track while L is set.
- The recording starts on the first sample whose level reaches the threshold, back-dated by a fixed
  10 ms pre-roll from the input history so the attack is kept.
- *Claude's call:* the close of the first loop stays back-dated by Trigger latency. Only the start
  uses the pre-roll. Today `closeFirst` reuses the start's back-date, so this needs separate values.
- *Claude's call:* only one track can be armed, and arming behaves like recording. The press closes
  any track that is recording or overdubbing right away, as B does today. A B press on another track
  cancels the arm and then does its own action.
- The arm is cancelled by B on the armed track, a tap on C, a stop, Clear track, or Clear all.

### Display

| | Widget text | B label | B LED |
|---|---|---|---|
| Armed | `ARM` | `ARM` | red, slow blink |
| Stopping (End of loop / Fade) | `STOP`, blinking | `PLAY` | unchanged |
| Stopped | `STOP` | `PLAY` | off |

- *Claude's call:* the track marks stay as they are while stopped, so ▶/■ still show what will play
  on restart. The progress bar stays empty, with the playhead at the top. An armed track's empty dot
  blinks.
- C's label stays `UNDO`/`-`.

## Ports

Settings: Stop mode (Instant / End of loop / Fade), Fade time (s), Auto Rec, Threshold (dB) and
Double-tap (ms, about 150–600).

*Claude's call:* a **Stop/Play** trigger in the Perform group, for the web UI or a footswitch on another
page. It stops (with the Stop mode) while running and restarts while stopped.

The new ports go after the existing ones, with bypass kept last. Existing symbols are unchanged. The
plugin is unreleased and MOD saves pedalboards by symbol, so this breaks nothing. The group assignment
in `initParameter` has to place Stop/Play in Perform.

## Engine notes (decided)

- The transport is engine state: running, stopping (one of the three modes), or stopped. It lives in
  `Looper`, because only the engine knows the playhead, the writers and the pending changes.
- **Restart-safe pending changes:** a pending op owes one stretch of the loop and applies only while
  the playhead is inside it.
  - At the restart, an op whose owed stretch wraps past the top splits in two: one part starts at the
    top, the other waits for its start. Delta ownership or refs go to the part that ends last.
  - The op slots need room for the splits.
  - The 15 ms op ramp is not needed for a split part, because the playhead only ever reads audio that
    has already been applied.
- End of loop parks exactly at the top, so its ops continue without a split.
- Auto-record detection runs per sample inside `process()`, which already owns the history ring.

## Testing

- **Engine:**
  - each stop mode ends in silence, with no sample-to-sample step above the existing click bound;
  - End of loop parks at position 0;
  - restart plays from buf[0];
  - undo, stop mid-pass, restart: the buffer after the next pass equals a run without the stop (the
    same for the zero op of a recording onto the loop);
  - a tail still running at the stop is complete in the buffer;
  - auto-record starts at the crossing minus 10 ms;
  - the first loop's L uses Trigger latency at the close;
  - arm cancel paths.
- **Plugin:**
  - tap undo fires after the window;
  - double-tap stops on the second press;
  - hold-redo is unchanged;
  - hold 2× while stopped clears;
  - double-tap then hold does not clear;
  - the Stop/Play port toggles.
- **HMI and widget:** the labels, LEDs and texts in the Display table.
- **Hardware, with the guitar:** the double-tap window, the threshold, and how Fade sounds.

## Left to the builder

- The engine API shape (stop/restart/arm calls and state queries) and the HMI view fields.
- The fade curve for Fade mode, and ranges and defaults within the ranges above.
- How End of loop handles a stop that arrives in the last 15 ms before the top, without a click.
