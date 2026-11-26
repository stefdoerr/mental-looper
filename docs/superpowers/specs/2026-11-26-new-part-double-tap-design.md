# B double-tap: a new part from here (design)

Status: approved, 2026-11-26 (after the builder check). Decisions marked *my call* were
proposed by me and accepted in one "sounds good", or settled by me after the builder check (marked *my call, after the
check*); they are open to correction. Everything else was decided by the user.

## Why

The user: "honestly one thing I'm not sure about is the parts UX". What the code and the manual show:

- **Changing part takes your hands off the instrument.** Part is on knob subpage 2: change subpage, turn the knob,
  change back. Every looper compared has a footswitch for it: the Aeros Loop Studio has a part switch (2x2 mode), the
  Echoplex has NextLoop, the Ditto X4 in serial mode switches loop per footswitch.
- **A fresh part is expensive.** Entering an empty part copies every track of the part you left. To build a chorus from
  scratch you then hold C on each track to clear it.
- **A new part's first recording needs a well-timed B.** You press B after the switch lands. The Aeros ("Record New
  Part") and the Echoplex (AutoRecord) start recording at the transition instead; the Echoplex also chooses per switch
  whether to copy the loop's sound, only its length, or nothing (LoopCopy).

Success: with tracks 1–3 playing in part A, select track 3 and double-tap B. At the end of A's pass the looper is in
part B, drums and bass (tracks 1–2) carry on, and track 3 records the chorus chords from B's first beat. Close it with
B as usual. No subpage, no knob, no clearing.

## Goals

- **A double-tap on B goes to the next part** (A→B→C→D→A), switching at the end of the current pass as Part does.
- **If that part is empty, it starts from the selected track:** the tracks below the selected one are copied over, the
  selected track records from the new part's first beat, and the tracks above stay empty. So track numbers are layers:
  track 1 is the foundation, each track builds on the ones below. Selecting track 1 gives a clean part.
- **If that part holds audio, the double-tap just switches to it.**
- B's single tap keeps acting on the press, exactly as today.

## Non-goals

- Going to a chosen part by foot (only "the next one"). The Part knob stays for that.
- Changing what the Part knob does: turning it to an empty part still copies every track (*my call*: the double-tap is
  "a new part from here", the knob is navigation).
- A setting for copying (Echoplex's LoopCopy), learning a song from live switches, the Track knob running across parts.
  See Later.
- Changing the song, Sync, saved pieces, or the port list.

## Anti-goals

- B's single tap acting any later than today. Recording timing must not move, so B never waits to see whether a second
  press comes: the first press acts, and the second deals with what it did.
- The copies of the tracks above the selected one being heard at the switch, even briefly (no copy-then-clear).
- The new recording starting off the new part's first sample.
- Cost in the audio loop. The user's CPU rule (for tempo sync, applied to every feature since): "it should not add
  significant CPU cost to our plugin especially when not used". The gesture is block-rate control logic; the engine
  adds work only at a part switch.

## Constraints

- The Dwarf's page 1 has footswitches B and C and three knobs; A changes page. B and C are full: B taps and holds, C
  taps, holds and double-taps. B has no double-tap today, which is why it can take this.
- The port order and symbols are frozen since v0.1.0 (released). This needs no new port.
- Gestures are timed in whole audio blocks, as B's hold and C's gestures are today.
- Any DPF change stays in the local patch (none is expected).

## Approaches considered

1. **The engine does the switch, the partial copy and the recording start in one step; the plugin recognises the
   double-tap (chosen).** The engine already switches parts on the exact sample (`switchPart()` at the part end) and
   copies a part there (`copy()`); starting a recording onto the loop at that sample is the same step a one-press
   handover takes. The plugin already turns C's presses into taps, holds and double-taps.
2. **The plugin drives it with existing calls** (queue the part, then clear the upper tracks and press record once the
   switch lands). Rejected: the switch copies every track, so the unwanted copies would sound and fade out at the
   switch, and the plugin acts between audio blocks, so the recording would start up to a block (2.7 ms at 128 frames)
   after the downbeat.

## Behaviour

### The gesture

- A **double-tap** is a second press on B within the Double-Tap window (the setting C's double-tap uses, default
  400 ms), timed as C's is.
- It acts on the track selected at the first press, even if Track turns in between, and belongs to the part of the
  first press: if the part changes before the second press, the second press is a new first press. (Both as C's
  gestures today.)
- A first press held past Hold Time is an undo (as today) and starts no double-tap: the next press is a new first press
  (*my call*).
- The second press acts on the press. Holding it does nothing more (*my call*: no undo).
- A press on C between the two presses makes B's second press a new first press, as a press on B already does between
  C's (*my call*, symmetric).
- A third press is a new first press.
- During a load, presses are ignored, as today.

### The second press first deals with what the first press did

As B's hold does (*my call*):

| The first press | The second press |
|---|---|
| started an overdub | cancels it: nothing is kept (as the web UNDO cancels a running overdub) |
| closed an overdub | the overdub stays |
| started a recording onto the loop, or the first recording | aborts it: the track is empty again |
| closed a recording (the first, or onto the loop) | it stays. With After Rec = Rec > Dub, the overdub the close started is cancelled |
| a one-press handover (closed another track's take, started this one's) | the other take stays closed; this one's start is undone as above |
| armed an empty track (Auto Rec), or queued a wait to start a recording (Sync) | the arm or wait is cancelled |
| queued a wait to close the first recording (Sync, more than 150 ms after a bar line) | the close stays: the loop closes on the line, and the switch comes at the end of that first pass. If the press was a one-press handover, only the other track's start is dropped (*my call, after the check*: a waiting close is a close that has not landed yet, and a closed take stays) |
| cancelled a wait, an arm or a pending double-tap switch | nothing to undo |
| cancelled an End of loop or Fade stop | the stop stays cancelled |
| started everything from stopped | see While stopped |

Then it queues the next part.

### The next part

- **Next** is the part after the current one (the part playing now), D wrapping to A. A switch already queued by the
  Part knob is replaced (*my call*).
- **Empty** means every track empty, the test today's copy on entry uses.
- The switch happens when today's queued switch would: at the end of the current part's pass (its longest track), or at
  once when stopped or with nothing recorded.

**The next part is empty.** At the switch, in one step on the switch sample:

- The tracks below the selected track t are copied from the part being left, as copy on entry copies today: the audio
  as heard, one layer, no undo history.
- Tracks t and above get no audio.
- Every track's on/off and volume come from the part being left, as today's copy gives them (*my call*); track t is set
  on, so its recording is heard.
- Track t starts recording on the switch sample, with no back-date: it is a recording onto the loop (long-track rules:
  it may run several loops, its length counts from this start, Snap applies; it starts on a loop top, so on its
  beginning). Close it with B.
- With Auto Rec on, track t is armed at the switch instead, and records from your first note, as B's tap would arm it
  (*my call*).
- A take still running in the part being left closes at the switch, as `switchPart()` closes it today. The new
  recording takes the input from the same sample without gap or doubling, as a one-press handover does.

**The next part holds audio.** The double-tap queues a plain switch to it, as turning Part to it would: no copy, no
recording.

### While the switch waits

- **A new-part recording waiting:** B blinks fast red, as before a recording that waits for a Sync bar line. The Loop
  widget shows the queued part (`A>B`, blinking, as today), and the web face shows its waiting look (the mouth
  trembling half open).
- **Cancel:** a tap on B, or a press on C, cancels the switch and its recording and does nothing else. A second press
  within the Double-Tap window after that cancelling press is a double-tap again (*my call*, as C's cancelling press
  works today).
- Turning Part to another part replaces it with a plain switch to that part. Turning Part back to the current part
  cancels it (as today).
- Turning Track does not move the recording to another track.
- **A plain switch waiting** (the next part held audio) behaves exactly as a switch queued with the Part knob: B and C
  act as usual and do not cancel it (*my call*: only a waiting recording shows a wait to cancel).
- **A stop of any kind cancels it,** switch and recording, when the stop begins: Stop/Play, MOD's stop with Sync,
  an End of loop or Fade stop. (A song's Stop step cannot come: the double-tap turned Song off.) C's double-tap is no
  exception: its first press already cancels the wait, as C's cancelling press does today, and its second press stops,
  in the current part. So a stop never leaves a half-made new part (*my call, after the check*; a switch queued with
  the Part knob still happens at the park, as today).
- **Clear All** cancels it.

### With the song

The double-tap turns Song off and queues the next part, as turning Part by hand does today. "Next" is after the part
playing now.

### While stopped

The first press already started everything from the top (B's tap while stopped), possibly waiting for a bar line with
Sync. The second press replaces that start (*my call*):

- The loops stop again at once (with the usual short ramp), the next part becomes the current one at once, and
  playback starts from its top, the new part's recording (or arm) starting with it. With Sync, that start waits for the
  bar line as B's start does.
- The second press starts playback whatever the first did: if the first press only cancelled a start that was
  waiting for a bar line, the second still switches and starts, with the recording (*my call, after the check*).
- If the next part holds audio, it starts playing it, with no recording.
- If that start waits for a bar line (Sync) and the wait is cancelled (a tap on B, a press on C), the looper stays
  stopped in the new part with its copies, and nothing records. The switch has happened and counted as a change
  (*my call, after the check*).
- With Song on, the first press started the song from step 1; the second turns Song off, and "next" is after step 1's
  part.

### With nothing recorded

The first press started (or armed) the first recording; the second aborts it. With nothing recorded the switch happens
at once, and the empty next part starts its first recording on track t (or arms it), as B's tap would. So a double-tap
on an empty looper just moves to the next part and records there (*my call*).

### Saved pieces

The copies and the new recording count as changes: a later Load asks `LOAD n?`, as any change does today.

## On the Dwarf and in the web face

- **B's light:** fast red blink while a new-part recording waits; otherwise as today.
- **B's label:** `REC` while a new-part recording waits (the next thing B does is close what will record) (*my call*).
- **Loop widget:** the queued `A>B` as today; when the switch lands, the new part's track shows `REC` (`B3`).
- **Web face:** the part keys show the queued part, as they do for a knob switch today; the mouth's waiting look while a
  new-part recording waits.

## Settings

- **Double-Tap** (150–600 ms, default 400 ms): now the window for a second press on B and on C. The manual's line "B
  has no double-tap" goes.

No new setting, no new port.

## Manual

- Footswitch B: a double-tap row in the table, a paragraph on "a new part from here" with the example from Success,
  and the table of what the second press does with the first.
- Song parts: switching by foot, and the layer rule (below copied, selected records, above empty).
- Settings: Double-Tap.
- README: one line in the parts bullet.

## Review focus (failure modes no rule above names)

1. A double-tap whose first press made a one-press handover: the closed take keeps its punch-out fade, and undoing the
   new start leaves no gap and no doubled input in it.
2. The switch sample: the new recording's first sample is the new part's first sample, while the old part ramps out
   and the copies below ramp in from their beginnings.
3. Copies below that are long tracks: the new part's pass is its longest track, and changes when the new recording
   closes several loops long.
4. Memory full at the switch: the recording closes at once as at the 5-minute limit (FULL), and the copies (shared
   blocks) still play.
5. A Save while the switch waits, and a Load or a pedalboard change in the same window.
6. A double-tap on the first recording with Sync: the close waits for the line while the new part waits for the end
   of the pass that close creates. Two waits at once, where today one action waits at a time.

## Left to the builder

- The engine call's shape (the queued part plus the track to record), and how it sits next to `setPart()` and the Sync
  pending actions.
- How B's double-tap shares the gesture code with C's.
- Tests: engine tests for the switch step (sample-exact start, copies below, nothing above, on/off and volume, Auto
  Rec arm, long copies), plugin tests for the gesture (each row of the table, cancel, Track and Part turned between
  presses, Song, stopped, nothing recorded, a plain switch to a part with audio, D to A).
- A bench on the Dwarf at the real flags against main before merging (the CPU rule).

## Later

- The Echoplex's per-switch choice of what to copy, or a setting for whether the knob's switch copies.
- A "learn" mode that fills the song's steps from the switches made live (parts and passes).
- The Track knob running across parts (A1–D4), parked on 2026-11-26.
