# Footswitches B and C (design)

Status: approved, 2026-11-25. Decisions marked *my call* were proposed by me and not argued; they are open to
correction. Everything else was decided by the user.

## Why

The user, on the Dwarf: "multiple undos can get annoying, and clearing a whole loop is very tiring", and "it's
confusing how you start them all after loading". What the code shows:

- No footswitch clears one track. Clear Track exists only as a web button, so clearing a track on the Dwarf means
  tapping undo once per layer.
- C's tap undoes only after the Double-Tap window (400 ms), and two quick taps are a double-tap, which stops
  everything. Peeling layers is slow, and tapping faster stops the loops.
- B already starts everything after a load (its label reads PLAY), and so does a double-tap on C, but nothing on the
  Dwarf says so unless you know.

Other loopers split two switches the same way (TC Ditto X2, Boss RC-1 and RC-500, Sheeran Looper X): the record
switch undoes on a hold, the second switch stops and clears on a hold, and on multi-track units a hold clears one track.

Success: on the Dwarf alone, drop the selected track out on the beat and bring it back; clear a track with one hold;
peel layers with holds on B without ever stopping the loops by accident; after a load, start with one track and build
up, or start everything.

## Goals

- **B** keeps every tap action it has today, acting on the press. **Holding B undoes** one layer of the selected track;
  each further hold peels one more.
- **C** acts on the selected track: a tap turns it off or on, a hold clears it, a double-tap stops or starts
  everything, and a long hold while stopped clears everything.
- **Redo** moves to knob 2's press.
- After a load (or any stop): B or a double-tap on C starts everything; a tap on C starts only the selected track.

## Non-goals

- Redo by foot (rare enough for a knob).
- An undoable Clear Track (the EHX 95000 has one). Later, if clearing by foot proves risky.
- A true per-track stop, where a track halts and restarts out of time with the others. "Off" keeps it running in time,
  as On/Off does today.
- A setting to choose between the old and the new layout.
- Changing Song, Sync, Auto Rec, saved pieces or the web interface beyond what follows from the ports below.

## Anti-goals

- B's press acting any later than today: recording timing must not move.
- A hold on C letting a track sound before it clears.
- A hold on B emptying a track.
- Any cost in the audio loop while playing. The user's CPU rule (for tempo sync, applied to every feature since): "it
  should not add significant CPU cost to our plugin especially when not used". Gestures are block-rate control logic.

## Constraints

- The Dwarf's page 1 has footswitches B and C and three knobs; A changes page. The user assigns ports to them in
  MOD-UI.
- A port can report a release only if it is a momentary toggle (`mod:preferMomentaryOnByDefault`, as `undo_redo`
  is today); a trigger port reports presses only. B's port must become momentary. This needs no DPF change (any DPF
  change stays in the local patch; none is expected).
- Gestures are timed in whole audio blocks, as C's are today (at most one block of error).
- Not in production: port symbols, names and saved-pedalboard compatibility may change freely.

## Approaches considered

1. **Gestures in the plugin, small engine pieces (chosen).** The plugin already turns C's press and release into tap,
   hold and double-tap with Hold Time and the Double-Tap window; that extends to B and to C's new actions. The engine
   gains an undo that never takes a track's first layer and "start from the top with only this track on"; Clear Track,
   Clear All, On/Off and restart exist. The audio loop is untouched.
2. **A setting to switch between the old and the new layout.** Rejected: twice the behaviours, tests and manual, and
   nothing is in use that needs the old one.
3. **MOD's own assignments only** (Clear Track, Undo and On/Off on switches). Rejected: one function per switch, no
   tap/hold/double-tap, so two switches cannot carry it.

## Behaviour

"The selected track" is the one knob 1 (Track) picks. A gesture acts on the track selected at its first press, even if
Track turns before it ends (*my call*).

A gesture also belongs to the part it was pressed in. If the part changes before the gesture ends (a part switch, a
song step), the rest of it is dropped: no release action, no hold, and a second press counts as a new first press. B's
hold likewise does nothing if the part changed since its press (*my call*: a gesture never clears or changes a track in
a part the user was not on when pressing). While stopped, "the selected track" and "empty" are judged in the part that
will start (with Song on, step 1's), and C's label follows that part (*my call*).

### B: Rec/Undo

- **Tap:** exactly as today, on the press: record, close, overdub, play; one-press handover; arm with Auto Rec; wait for
  the bar line with Sync; start everything from the top while stopped (with Song on, from step 1).
- **Hold** (Hold Time, default 500 ms): undoes the newest layer of the selected track. Each further hold undoes one
  more. **Never the first layer:** with only the first recording left, a hold does nothing. A hold never aborts a
  recording.
- What the hold does with what its own press started:

  | The press | At the hold |
  |---|---|
  | started an overdub | that overdub is dropped (nothing kept), then the newest layer before it is undone |
  | closed an overdub | that overdub is undone (it is the newest layer) |
  | queued a wait for a bar line (Sync) | the wait is cancelled, then one layer is undone |
  | started or closed a recording | the recording stays; nothing is undone (it is the first layer). With After Rec = Rec > Dub, the overdub the closing press started is dropped |
  | armed an empty track (Auto Rec) | the arm stays; nothing else happens (*my call*) |
  | started everything from stopped | playback runs; one layer of the selected track is undone |

- **Redo after holds brings back only the last hold's layer** (decided by the user). Each hold's press starts an overdub
  on a playing track, and starting an overdub clears what was waiting to be redone (`startDub` calls `clearRedo`), so
  after three holds only the third layer can come back. No engine change; the manual says so.
- The label is unchanged (REC, DUB, PLAY, ARM, as today).

### C: Stop/Clear

**Rule of thumb:** C turns sound off on the press and turns it on when you lift your foot, so a hold never lets a track
sound before it clears.

While the loops run (or a Stop Mode stop is finishing):

- **Press on a track that is on:** turns it off at once (the usual On/Off ramp). On 1–4 follow.
- **Release, before the hold time, on a track that was off:** turns it on.
- **Hold:** at the hold time the selected track clears, as Clear Track does: it goes empty and ramps out; it cannot be
  undone. The cleared track is set back to on, so the next recording on it is heard (*my call*).
- **Double-tap** (a second press within the Double-Tap window): puts back what the first tap did, then stops
  everything with the Stop Mode. During an End of loop or Fade stop, a double-tap stops at once (as today).
- **An empty selected track:** a tap does nothing (it would leave the next recording silent) (*my call*); a double-tap
  still stops everything.

While everything is stopped:

- **Tap** (released before the hold time): starts playback from the top with only the selected track on. Every other
  track of the part that starts is turned off (On 1–4 follow). With Song on, from step 1, as B. An empty selected
  track: nothing (*my call*).
- **Double-tap:** puts back what the first tap did and starts everything from the top, as B does, on the second press.
  (The first tap may already have started the selected track; the second press restarts everything from the top.)
- **Hold:** at the hold time the selected track clears; keep holding to twice the hold time and everything clears
  (Clear All, as today: loops, loop length, every part). Nothing starts.

During a take:

- **The first recording** (no loop yet): as today, a tap aborts it (the track is empty again) and a double-tap keeps it
  as the loop and stops silently. Here the tap still waits out the Double-Tap window, as today: nothing plays, so
  nothing is late. A hold aborts it at the hold time.
- **The selected track records onto the loop or overdubs:** the press closes the take as B would (it is kept) and turns
  the track off. A hold then clears the track, take included. A double-tap puts the track back on and stops everything.
- **Another track's take:** unaffected; C acts on the selected track.

Waits and arms: a press while the selected track's wait for a bar line or Auto Rec arm, or a pending restart (which
belongs to every track), is pending cancels it and does nothing else; a second press within the window still stops
everything (as today). Another track's wait or arm is left alone, and the press acts on the selected track as usual
(*my call*: C acts on the selected track, and B's ARM and WAIT labels follow only the selected track, so cancelling
another track's would be invisible).

With Sync, C's tap while stopped starts a restart that may wait for the bar line (WAIT). The other tracks turn off at
the tap. If that wait is cancelled before playback starts (B, a press on C), they are put back on, so the next start
plays everything. A second press within the Double-Tap window during the wait puts them back on and leaves the start
waiting: everything starts on the line (*my call*; plugin-side, no new kind of pending action in the engine).

During a load: presses are ignored, as today.

### Knobs (page 1)

- Knob 1: Track (unchanged).
- **Knob 2 press: Redo** of the selected track (was On/Off, now C's tap). After holds on B, it brings back only the
  last hold's layer (see B).
- Knob 3: Volume (unchanged).

These are assignments the user makes in MOD-UI; the manual names them.

### Labels on the Dwarf

- B: unchanged.
- C: while running, `ON n` or `OFF n` for the selected track n; while stopped, `PLAY n`; `-` when a tap would do
  nothing (an empty selected track) (*my call*).
- Knob 2: `REDO`, or `-` when there is nothing to redo, as C's `UNDO` / `-` does today (*my call*).

### Settings

- **Hold Time** (200–2000 ms, default 500 ms): B's and C's holds. Twice it, held on C while stopped, clears everything.
- **Double-Tap** (150–600 ms, default 400 ms): the window for C's second press. B has no double-tap.
- **Stop Mode:** "how a double-tap on C stops", unchanged.

### Ports

- `rec` (name *Rec/Undo*): becomes a momentary switch instead of a trigger. The web REC button already sends press and
  release.
- `undo_redo` becomes `stop_clear` (name *Stop/Clear*), momentary as now.
- `onoff`, `undo`, `redo`, `clear_track`, `clear_all` and `stop_play` stay as they are, for the web buttons and other
  assignments. The web UNDO keeps today's undo, which can take the first layer (*my call*).

### Manual and README

Quick start, the Footswitch B and C sections, knob 2, the Hold Time, Double-Tap, Stop Mode, Auto Rec and Sync (WAIT)
wording, and Saved pieces ("B or a double-tap on C starts everything; a tap on C starts the selected track alone").

## Review focus (failure modes no rule above names)

1. A hold on B whose press landed right at a take's close, with After Rec on Rec > Dub, or one-press handover from
   another track.
2. Track turned between the taps of a double-tap, or during a hold (the first press's track keeps the gesture).
3. A double-tap whose first tap turned a track off just before a Fade or End of loop stop: the track must be on for
   the fade-out, not silent.
4. Saved pieces: C's actions change On 1–4 and count as changes (a later Load asks `LOAD n?`), and a hold that clears
   during a Save's settling.

## Left to the builder

- How B and C share the gesture code, and the block timing (as today).
- LED colours for C.
- Tests: plugin-level gesture tests replace today's C tests.

## Later

- An undoable Clear Track.
- Per-track input select (two instruments), parked from the stereo discussion.
