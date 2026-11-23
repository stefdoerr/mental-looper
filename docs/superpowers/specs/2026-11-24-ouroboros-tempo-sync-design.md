# Design: Ouroboros — tempo sync

Builds on the looper, stop/clear/auto-record and longer-tracks specs in this folder. Everything not named here
stays as it is.

## Why

Many MOD players loop over a drum machine or a drum plugin that follows MOD's tempo. They need loops that start
on the downbeat, are whole bars long, and start and stop together with the drums. This is the second of three
features for that wider audience, after longer tracks and before song parts. The user plays in free time
himself, so sync is an option, and it is off by default.

## Goals (decided by the user)

- **A setting, Sync: Off / Bars.** With it off, Ouroboros behaves exactly as today.
- **Quantized to the bar (option A).** Starting a recording and closing the first loop snap to MOD's bar lines,
  so every loop is a whole number of bars.
- **Follows MOD's play/stop.**
  - When MOD's transport starts, every track plays from its top on bar 1.
  - When MOD's transport stops, the loops stop.

## Non-goals

- **Setting MOD's tempo from a loop.** mod-host has no path from a plugin to the global tempo. The LP3 leads by
  sending MIDI clock out. A clock output like that could come later.
- **Time-stretching.** Loops keep their length when the tempo changes (effects are out of scope).
- **Quantized overdubs, beat-level quantize, fixed-length recording ("choose N bars").** That last one could come
  later as an option.

## Anti-goals

- **Downbeat jitter.** A loop that starts a block late against the drums.
- **Lost feet.** A slightly late press that misses the bar and waits a whole extra bar.
- **Changed free play.** Any change at all to Sync Off behaviour.

## Approaches

1. **The engine schedules presses to the exact sample (chosen).** Each block the plugin passes the engine the
   bar grid (where the next bar line falls, and the bar length in samples) from MOD's time position.
   - The engine holds a quantized action until that sample.
   - A late press within the grace window is back-dated to the line from the input history, like the existing
     trigger-latency back-dating.
2. **The plugin delays the call until the block containing the bar line.** Rejected: the action lands up to one
   block late (2.7 ms at 128 frames, more at larger buffers).
3. **Syncing to MIDI clock directly.** Rejected: MOD's transport already merges MIDI clock, Ableton Link, tap
   tempo and the web UI's tempo, so reading it covers them all.

Compared with the LP3 (its plugin manual, "Host Sync"):
- The LP3 starts at the press and rounds the length up to whole bars, keeping the start's offset from the bar.
- The LP3 does not follow play/stop.
- Ouroboros starts on the downbeat and follows play/stop: the two things LP3 users asked for on the MOD forum.

## Behaviour

### The bar grid

- **Taken from MOD's time position:** the tempo, the beats per bar, the play state, and the position in the bar
  at the start of each block. DPF extrapolates this position between host updates.
- **One bar** is beats per bar × 60 / tempo seconds. *Claude's call:* the time signature's denominator is
  ignored, as the LP3 does.
- **While MOD's transport plays,** the grid is MOD's bars.
- **While it is stopped:**
  - before anything is recorded, the grid starts at the first quantized press, which becomes bar 1, and then
    advances at MOD's tempo;
  - once a loop exists, its bars are the grid.
  - *Claude's call, decided with the user ("the bar grid starts at your first press").*

### What snaps (Sync = Bars)

- **These snap to bar lines:**
  - starting the first recording;
  - closing the first recording, so L is a whole number of bars;
  - starting a recording onto the loop, which can become a long track;
  - an auto-record start (it fires on the bar line after the threshold is crossed, back-dated within the grace
    window like a press);
  - a restart from stopped while MOD's transport plays (B or a double-tap).
- **Early or late:**
  - **An early press** (before the line) waits for the line.
  - **A late press** up to `kSyncGraceMs = 150` ms after the line counts as on it: the action is back-dated to the
    line from the input history. The history grows from 100 ms to at least 250 ms.
- **These stay immediate:**
  - closing a recording onto the loop: it keeps the longer-tracks rule, whole loops from its start, and since its
    start was on a bar line and L is whole bars, it stays on the bar grid;
  - overdub punch-in and punch-out;
  - undo, redo, on/off and clear;
  - the double-tap stop. End of loop already ends on a loop line, which is a bar line.
- **While an action waits:**
  - **Footswitch B:** B's LED blinks fast in the colour of what's coming (red: a recording starts; green: the first
    loop closes and plays). The label keeps showing that coming action.
  - **Screen:** the widget text shows `WAIT`. *Claude's call.* The font gets W and I.
  - **Pressing again:** a second B press while waiting cancels the pending action. *Claude's call.*
  - **Undo:** a tap on C while waiting cancels it too. *Claude's call.*

### Following MOD's play/stop (Sync = Bars)

- **MOD starts** (play state off → on): every non-empty track restarts from the top, the longest on its
  beginning, exactly on the bar line where MOD starts. If the looper was playing, that is a restart; if stopped,
  it starts. Empty tracks are unaffected, and recording is possible either way.
- **MOD stops:** the loops stop, Instant. *Claude's call:* follows the Stop Mode setting, so End of loop or Fade
  apply here too.
- **No effect on recordings in progress:**
  - a first recording or a recording onto the loop in progress when MOD starts is not restarted, because restart
    only acts on non-empty playing tracks;
  - when MOD stops, a recording is closed, as with any stop.
- **Your own stop and restart still work:** stop with a double-tap while MOD plays, and B restarts on the next bar
  line.

### Tempo changes

- **The grid follows MOD's tempo at once.**
- **Recorded loops keep their length,** and drift from the drums after a tempo change. The manual says so.
- **A tempo change during the first recording:** L is whole bars at the tempo when it closes.

## Edge cases (Claude's calls, from the builder check)

- **MOD starts while the loops are already playing.** The user's rule applies: every track plays from its top.
  - The engine does what an Instant stop and a restart do: close any recording or overdub (as a stop does) and
    move every playhead with the park's alignment (the longest on its beginning, the others shifted equally, one-loop
    tracks to the top).
  - It does all this at the line, with a 3 ms crossfade instead of the stop's 15 ms fade. The old position fades out
    while the top fades in, so the downbeat stays exact and nothing clicks.
  - This is a new engine operation, "retrigger at sample N".
- **What counts as MOD's play edge.**
  - Only a seen change of the play state from off to on, while Sync is Bars.
  - The first block after loading or after Sync is switched on is not an edge, so changing a setting never jumps the
    loops.
  - A relocate or loop-back while playing is not an edge.
  - If MOD starts at bar 1 beat 1, the retrigger happens on that sample. If it starts mid-bar (a resume, a Link or
    MIDI-clock start), the retrigger waits for the next bar line.
- **The first recording keeps the grid it started on until it closes.** If MOD starts during it, that edge is
  ignored for the looper.
  - Its bars are counted as the samples recorded since its start line, divided by the bar length at the close,
    rounded to the nearest whole number, with at least 1 bar.
  - The close waits for, or back-dates to, a line of that grid.
- **The grid while a loop exists:**
  - while MOD plays, MOD's bars;
  - while MOD is stopped, the loop's own bars. The engine stores the first loop's bar count, so a bar is L divided
    by that count, with lines from the loop top;
  - a loop recorded with Sync Off counts as one bar.
  - After a tempo change, MOD's lines no longer meet the loop's: a recording started on a MOD line then stays k·L
    long from its own start (the accepted drift).
- **No usable grid** (DPF's `bbt.valid` false, or a host that sends no tempo): every press acts at once, as with
  Sync Off. A footswitch never goes dead. MOD is the target, and desktop hosts that send a tempo work the same way.
- **MOD stops during the first recording.**
  - If at least one whole bar has been recorded, it closes on the last bar line passed. The rest is dropped: it was
    never heard, since a stop that closes a first recording parks silently.
  - Otherwise the recording is discarded.
- **B on stopped loops while MOD is stopped:** restarts at once (the loop's own grid starts at its top anyway).
- **Auto-record with Sync:**
  - The bar line wins.
  - A threshold crossing within the grace window after a line starts at that line.
  - An earlier crossing waits for the next line, and the pre-roll is kept as a lead-in just before the line (it wraps
    to the loop's end, as lead-ins already do), so an attack just before the downbeat isn't lost.

## Engine notes (Claude's calls)

- **The plugin passes the grid each block, before `process()`:** valid or not, the samples from the block start to
  the next bar line (fractional), and the samples per bar (fractional).
  - With Sync Off it passes "no grid", and the engine behaves exactly as today.
  - The engine advances its own copy per sample within the block.
- **A quantized action is pending state in the engine:** what to do (a press on track t, or a restart) and the
  sample it fires at.
- **A press within the grace window after a line fires at once,** back-dated by the samples since the line. This
  replaces the trigger latency for that press: the line is the intended moment.
- **The first loop's close lands on the line.** `closeFirst` with the close back-dated to the line: the samples
  recorded past it become its tail, as today. `L = round(bars × samples per bar)`.
  - *ponytail:* rounding to whole samples drifts from MOD's bars by under a sample per bar. Follow-play
    re-aligns at every MOD start.
- **Port:**
  - `sync` (enum Off/Bars, default Off) goes in Settings, after the existing ports and before bypass.
  - `DISTRHO_PLUGIN_WANT_TIMEPOS` adds DPF's `lv2_events_in` port between the audio ports and the parameters.
    `paramFromLv2Index` and the `static_assert` change accordingly.
  - Saved pedalboards are unaffected: MOD stores ports by symbol.
- **RT-safety:** a few doubles per block and a compare per sample. Nothing allocates.

## Testing

- **Engine** (a synthetic grid, no DPF):
  - an early press waits and starts exactly on the line;
  - a late press within 150 ms starts back-dated exactly to the line; one later than that waits for the next
    line;
  - L is a whole number of bars;
  - a restart from stopped lands on the next line;
  - a second B or a tap on C cancels a waiting action;
  - Sync Off is bit-identical to today (an A/B on random sessions);
  - with the transport stopped, the grid starts at the first press.
- **Plugin** (DPF time position set in the test):
  - MOD's play edge restarts every track at the line and its stop edge stops them;
  - the `sync` port, its enum and the lv2 index mapping;
  - B's LED and label while waiting.
- **Widget:** `WAIT`.
- **Hardware:**
  - with a drum plugin following MOD's tempo, record 2- and 4-bar loops and check them against the drums over a
    few minutes;
  - start and stop MOD's transport;
  - change the tempo.

## Left to the builder

- The exact grid hand-off (struct or calls), as long as it is per block and sample-accurate.
- How the waiting LED blink is encoded, within the HMI extension.
- The widget layout of `WAIT`.
