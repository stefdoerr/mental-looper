# Design: Ouroboros — longer tracks

Builds on `2026-11-20-ouroboros-looper-design.md` and `2026-11-23-ouroboros-stop-clear-autorecord-design.md`.
Everything not named here stays as it is.

## Why

Today every track is exactly one loop long. Players want a long melody or a verse over a short groove, still
locked to the first loop. Aeros, RC-505, Ditto X4, Infinity 3 and the Looperlative LP1 all offer some form of
this. It's the first of three features meant to make Ouroboros useful to a wider audience. The other two, tempo
sync and song parts, come later and build on it.

## Goals

- **A track recorded onto an existing loop can be any whole number of loops long.** You just keep playing past
  the loop end, and the length comes from when you press B (decided by the user: option A, "record past the end").
- **Every track stays locked to the first loop**, pass after pass, through overdubs, undo, stop and restart.
- **Overdub, undo/redo, on/off, volume and clear work on a long track exactly as on a one-loop track**, over its
  full length.
- **No clicks**: the long track's own loop point is as smooth as the first loop's.

## Non-goals

- Multiply (doubling an existing track) and choosing a length before recording. Tempo sync may bring the
  second later.
- Tracks shorter than the first loop (½, ¼).
- Tempo sync and song parts (next rounds).

## Anti-goals

- A late foot losing what you played: the overshoot must end up in the track, not be cut off.
- Two long tracks drifting out of the alignment you recorded them in, after any number of passes or a restart.

## Approaches

How a long track gets made, as shown to the user:

1. **Record past the end** (LP3, Aeros). There's no setup and no new control. **Picked by the user**, because it
   fits free-time playing.
2. **Multiply** (Echoplex, LP1): double or triple an existing track and overdub on the repeats. It needs a new
   control, and the footswitches are full.
3. **Choose the length first** (RC-500, Ditto): predictable, but the least free. It's a candidate to add
   alongside tempo sync.

How the engine keeps tracks in step (Claude's call):

- **Per-track playheads advancing together (chosen).** Each track keeps its own position, wrapping at its own
  length, and all advance by one sample per sample. Since every length is a whole multiple of the first loop,
  they stay locked for good, and their relative alignment never changes because they all move together.
  Restarting shifts all of them by the same amount.
- **A global song counter with per-track offsets.** This needs either the least common multiple of all lengths
  (which explodes, e.g. ×7 and ×9) or a modulo per track per sample. More code for the same result.

## Behaviour

### Recording a long track (decided by the user)

- **Which tracks:** any track recorded onto an existing loop, meaning L is set and the track is empty. That
  includes an armed auto-record start and a one-press handover. The first recording still sets L freely, as
  today.
- **Recording runs past the loop end.** The track keeps recording pass after pass until it closes, up to the
  5-minute maximum. The other tracks keep playing.
- **B stops recording the moment you press it**, as today. Nothing you play after the press is recorded,
  except the short Tail ring-out below. Only the track's length rounds to a loop boundary (decided by the user
  after the builder check):
  - **Pressed within the first quarter of a pass** (the boundary just passed is no more than L/4 behind the
    press): the track ends on that boundary. Everything played past the boundary, up to the press, folds into
    the track's start, fading out the way the first loop's tail does. If that overshoot is shorter than the Tail
    setting, the input keeps blending live into the start for the rest of the Tail, like the first loop.
  - **Pressed later in a pass:** the track ends on the next boundary. The rest of that last pass is silent. The
    recording stops at the press with the usual 5 ms punch-out fade.
  - **Pressed before the first boundary** (still in the pass where recording started): the track is one loop
    long and the rest of the pass is silent, exactly as today. A long track is never shorter than one loop.
- **Handover, stop and the maximum** close the same way. There's no "carry on to the boundary" anywhere, so
  only one track ever takes the input (the existing single-writer rule).
- **The quarter is measured at the press,** the same instant the recording stops. *Claude's call.* Closes of a
  recording onto the loop are not back-dated today either.
- **The maximum:** a long track is at most `floor(5 min / L)` loops. At that boundary it closes as if B had been
  pressed there.
- **Where the recording sits:** a long track's cycle starts at the start of the loop pass in which you started
  recording it. What you played in that first pass before you started is silence. *Claude's call.* This keeps
  the recording exactly where you played it relative to every other track.

### Playing

- **Locked to the first loop:** a long track of k loops plays its content once every k passes, in the alignment
  it was recorded in.
- **Overdubs** on a long track circle its whole length. One press is one layer, as now.
- **Undo, redo, clear, on/off and volume** work as now. An undo is applied as the long track's playhead passes,
  over one pass of that track. It's still heard from the moment you press it.
- **After Rec = Rec > Dub:** closing a long recording goes straight into overdubbing over its full length, from
  the press.
- **Clearing** a track forgets its length. When every track is empty, L resets, as now.

### Stop and restart (decided by the user)

- **Restart from the top:**
  - the longest track starts from its beginning;
  - every other track keeps the alignment to it that it was recorded in;
  - one-loop tracks start from the top as today.
  - If several tracks share the longest length, take the lowest-numbered one. *Claude's call.*
- **End-of-loop stop:** waits for the end of the longest track's pass, then stops there. The fade happens over
  its last 15 ms, as today. *Claude's calls (builder check):*
  - The wait can be long (up to 5 minutes for a very long track), and that's intended: the whole arrangement
    ends together. STOP blinks on the screen meanwhile. A second double-tap stops at once, and B cancels, as
    today.
  - A stop that closes a recording measures the wait after that close, so a track that has just become the
    longest is waited for.
- **Instant and Fade stops:** unchanged. Parking moves every track's playhead as the restart rule above says,
  and pending undo/redo still finish correctly (the restart-safe ops of round 1), each over its own track's
  length.

### Screen (Claude's call)

- **The progress bar follows the selected track over its whole length**, with a small tick at each loop
  boundary inside it. That way you can see "pass 2 of 3". Leave the ticks out when there would be more than
  fit, i.e. fewer than 3 px apart.
- **While a long track is recording,** show the position in the current pass as today, plus enough to see how
  many passes are recorded so far. The exact pixels are left to the builder.
- **No new text:** the text row has no room for "×3".

## Engine notes (Claude's calls)

- **Each track gets `len`**, a whole multiple of L (0 while empty), **and `pos`**, its playhead.
  - Every track's `pos` advances each sample and wraps at its `len`.
  - The shared base phase `pos_` (position within the first loop) stays: every non-empty track has
    `pos ≡ pos_ (mod L)`.
  - Everything that today wraps at `L_` per track wraps at that track's `len` instead: `Writer::w`, delta
    offsets, `Op::at`/`Op::len`, `tailEnd`, the punch-out fade, the park split.
- **A recording onto the loop writes linearly from the base phase,** without wrapping at L, until it closes.
  - The track's index while recording is (passes so far × L) + `pos_`.
  - When it closes on boundary b, `len = b`. On a rounded-down close, the overshoot past b is folded into
    [0, overshoot) before the track plays.
- **A reused buffer must never play stale audio:**
  - while recording, the playhead position is zeroed before it's read (the writer is behind the playhead by
    the back-dating);
  - after the close, [0, start) is zeroed as the track plays, never in one block, because L can be long. Today's
    zero op covers one pass of L; extend the mechanism to the track's length.
- **The fold and live tail generalize `closeFirst`'s tail logic.** Overshoot h past boundary b:
  - fold `buf[b+i]` into `buf[i]` with a fade over max(T, h);
  - if h < T, `tailEnd` covers the rest of T live;
  - keep unity gain where it overlaps other writers (the `histUsed_` rules).
- **Pending undo/redo on a long track** lasts one pass of that track, up to 5 minutes, and holds its delta and
  an op slot until then. *Claude's call (builder check):* keep the existing 32-op cap. Rapid undo/redo past it
  within one long pass is ignored, as today within one loop. Mark it as a `ponytail:` ceiling. A foot doesn't
  press 32 undos within one pass.
- **Park and restart:**
  - the park sets the longest track's `pos` to 0 and shifts every track's `pos` by the same amount, modulo its
    own `len`;
  - the op split uses each track's `len`.
- **End-of-loop schedule:** the remaining samples are those of the longest track's pass.
- **RT-safety:** nothing new allocates. The per-sample cost is one increment and compare per track.

## Testing

- **Engine:**
  - **Length rounding:** a press within L/4 past a boundary rounds down; later rounds up with the rest of the
    pass silent; a press in the first pass gives one loop, as today.
  - **B stops at the press:** input after the press (beyond the Tail ring-out of a rounded-down close) is not in
    the track.
  - **The overshoot is in the track:** what was played past the boundary appears at the track's start.
  - **Seam:** maxStep across a long track's wrap stays within the existing bound, for both kinds of close.
  - **Lock:** after many passes the long track's content lines up with the base loop sample-exactly.
  - **Two long tracks of different lengths,** started on different passes, keep their alignment through 20
    passes, a stop and a restart.
  - **Overdub/undo/redo** on a long track restore exactly.
  - **A pending undo on a long track** survives stop/restart. This is the round 1 test, on a ×3 track.
  - **End-of-loop stop** waits for the longest track and parks with it at 0.
  - **Handover and stop** close with the same rule, and never two writers.
  - **The maximum** closes at the last boundary.
  - **A reused buffer** (clear, then record long) never plays old audio.
  - **Auto-record** onto the loop can produce a long track.
- **Plugin/HMI/widget:**
  - the bar follows a long track, with ticks at the boundaries;
- **Hardware, with the guitar:**
  - how the quarter-pass rule feels;
  - a long melody over a short loop, through stop and restart.

## Left to the builder

- The exact fold fade curve, as long as the seam test passes and the overshoot is kept.
- The widget pixels while recording a long track, and the tick style.
- How `pos_` and the per-track `pos` share code, as long as the lock and alignment tests pass.
- The order of the work: probably the length model and playheads first, then recording and closing, then
  stop/restart, then the screen.
