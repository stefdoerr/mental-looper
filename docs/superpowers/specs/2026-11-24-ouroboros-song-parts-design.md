# Song parts (design)

Status: draft for approval, 2026-11-24. Decisions marked *my call* were not discussed and are open to correction.

## Why

The user wants verse/chorus structure in Ouroboros: separate sets of loops per section, switched without breaking the
beat, and an arrangement that switches sections by itself so both hands stay on the instrument. They play a
Looperlative LP3 today (A/B parts) and like the way the Teenage Engineering K.O. arranges scenes into a song. This
follows tempo sync and the click, which made bar-locked building possible.

Success: build a song's parts on the Dwarf, arrange them, and play the whole piece without touching the pedal
between sections, with every switch landing on the beat.

## Goals

- **4 parts (A to D), each with its own 4 tracks.** Changes in one part never touch another.
- **A new part starts as a copy of the part you came from,** so the groove keeps playing while you build the next
  section.
- **Switching by hand,** queued to the end of the current part, seamless.
- **A song:** up to 16 steps, each a part played a number of times, ending in Stop or Loop. Edited on the Dwarf with
  knobs, shown on a widget.
- **The arrangement is saved with the pedalboard.** The loops are not, yet (see Later).

## Non-goals

- Saving loops (the next design; this one's storage must make it straightforward).
- Parts with lengths that are not whole multiples of the first loop, per-part tempo, more than 4 parts or tracks.
- Fills, transitions or per-part effects.
- An arrangement editor in the web UI beyond the plain controls.

## Anti-goals

- **A switch that breaks the beat:** a gap, an overlap or a click.
- **CPU growth for someone who never uses parts:** playing part A alone must cost what Ouroboros costs today, within
  measurement noise on the Dwarf (the CPU rule from tempo sync).
- **Silent loss of audio** when memory runs out.

## Constraints

- Real-time safe: no allocation, locks or unbounded work in run().
- The Dwarf has about 1 GB of RAM and no swap; about 767 MB was free with a pedalboard loaded. Each track today
  reserves 5 minutes up front (about 58 MB); 16 tracks that way would need about 925 MB and do not fit.
- What the Dwarf's controls can do (from the controller firmware and the device's hardware descriptor):
  - Footswitch A is the system's page key (tap: next page, hold: previous). B and C are Ouroboros's; pressing both
    together enters navigation mode. A page change swaps knobs and footswitches together, so song controls cannot
    live on another page.
  - Each page has 3 knob subpages, picked by hand with the three buttons above the knobs; B and C stay. Ouroboros
    uses subpage 1 (Track, On/Off, Volume).
  - One control per knob slot. A list turned on a knob changes its value directly when the device's "list click"
    option is off, or after a click when it is on (as Track does today). A toggle or trigger acts on a press.
  - A plugin widget can cover any rectangle of the screen on a given page and subpage; the knobs under it keep
    working.
- Everything that works today keeps working inside a part: undo/redo, long tracks, handover, stop modes, auto
  record, tempo sync, click and length.
- Saved pedalboards connect by symbol: existing symbols are frozen, new controls go before Bypass.
- No DPF changes are proposed upstream; any DPF change stays in the local patch.

## Approaches considered

1. **Scenes over the 4 shared tracks:** a part is a list of which tracks play. Small change, but only 4 tracks for
   the whole song, and a part differs from another only by mutes. Rejected by the user.
2. **Own 4 tracks per part, starting as copies (chosen).** Independent parts and 16 tracks of room, like the LP3's
   parts and the K.O.'s scenes. Costs a storage rewrite so 16 tracks fit in memory.
3. **LP3-style parts with free lengths:** each part's first recording sets its own length. Keeps the beat only with
   Sync on. Rejected.

## Behaviour

### Parts and lengths

- Ouroboros starts in part A. The first loop recorded in any part sets the loop length L for every part, and every
  track in every part is a whole multiple of L, as long tracks are today. So the beat holds across switches. L resets
  only when every track of every part is empty.
- A part lasts as long as its longest track, and a **pass** of the part is that length: it ends when the longest track
  reaches its end, as End of loop works today. Shorter tracks may be mid-pass then (a 2L track under a 3L one); that is
  fine, because every track of the next part, and of this one when it comes back, starts from its beginning. A part
  with no tracks lasts one L (silence).
- Only the current part's tracks play, record and take part in undo. The others keep their audio, frozen.
- **Copy on entry:** entering a part whose 4 tracks are all empty fills each of its tracks with what the matching
  track of the part you came from plays at the switch sample (after a take running there has closed): the audio as
  heard (every layer still in), its on/off and volume, its length. One layer each; no undo or redo history comes
  along. From then on the copies are independent. A section of silence is made by muting tracks, since an empty part
  is always filled on entry. *My call.*
- **A copy is all or nothing.** If it cannot be made in full (memory), the switch does not happen: the current part
  plays on, Part goes back to it, and the screen shows that memory is full. The song counts that step as played. The
  next entry tries again. The copy must not delay the switch; sharing audio until a copy changes is allowed (see the
  builder's part).

### Switching parts

- **The Part control (A to D)** picks the part to play next. The switch waits until the current part ends its pass
  (its longest track reaches its end), then the new part's tracks start from their beginning on that exact sample. Changing Part back to the current one before then cancels; only the last value counts.
- The outgoing part's tracks fade out over a short ramp so nothing clicks. The new part's tracks start at their top
  with their usual start handling.
- A recording or overdub running in the outgoing part closes at the switch, as a B press on that sample would. A
  waiting press or arm on the outgoing part is cancelled. *My call.*
- Stopped: the switch happens at once; the next restart plays the new part.
- **Loading a pedalboard:** Part, Song and the arrangement come back as saved; the loops do not (yet), so every part
  is empty. With Song on, Part is set to step 1's part; with Song off, the saved Part is where recording starts. *My
  call.*
- During the first recording (no L yet): the switch waits until the first loop is closed, then for the part's end.
  *My call.*
- With Sync on and MOD playing, part ends fall on bar lines because L is whole bars; nothing extra is needed.

### The song

- **Song (Off / On).** Off: Part switches by hand. On: the arrangement drives Part.
- **Steps:** up to 16, each with a part (A, B, C, D, Loop, Stop) and repeats (1 to 16). A step plays its part for that
  many passes of the part, then the next step's part follows at the part's end, by the same switch rules. Loop goes
  back to step 1. Stop stops every track with the Stop Mode at the end of the last pass. Steps after the first Loop or
  Stop are not played. Default: step 1 is A x1 and every other step is Loop, so turning Song on plays A on repeat.
- **Starting:** turning Song on while playing starts step 1 at the next part end. Any restart (B from stopped, a
  double-tap on C while stopped, Stop/Play, or MOD's play with Sync on) starts step 1 from the top while Song is on.
  Starting from nothing with Song on, the first loop is recorded in step 1's part and the song counts from the moment
  that loop closes, so a prepared arrangement can be played while its parts are built live. *My call.*
- **After the song:** a Stop step stops the loops and puts the song back on step 1; Song stays on, so the next restart
  plays the arrangement again. Clear All does the same: Song stays as it was, the song goes back to step 1.
  *My call.*
- **Hands on Part:** the plugin moves the Part control as the song advances, so the knob shows the part playing.
  Changing Part by hand while the song plays turns Song off and queues that part. *My call.*
- **Building during the song:** a step whose part is still empty is filled by copy on entry from the part before it,
  so a song can be built live on its first run, with the groove carrying on.
- Repeats count passes as long as the part is when each pass starts, so recording a long track mid-song is fine.

### Controls

- **Subpage 1, unchanged:** Track, On/Off and Volume act on the current part's tracks. The web UI's On 1-4 and Volume
  1-4 show the current part's tracks and update on a switch.
- **Subpage 2:** Part (A to D, a list like Track) and Song (Off / On, a press toggles it).
- **Subpage 3, the song screen:** Step (1 to 16), Step Part (A to D, Loop, Stop) and Step Repeats (1 to 16) edit the
  selected step. These are proxies, as the Volume knob is for the selected track. *My call:* the arrangement itself
  is not 32 visible controls; it is stored so that it is saved with the pedalboard (as plugin state or hidden ports,
  whichever DPF and MOD support cleanly; see the builder's part).
- **Loop widget (existing):** adds the current part's letter, `A>B` while a switch is queued, and the song position
  (for example `3/6`) while the song plays.
- **Song widget (new),** for the song screen: the steps with part and repeats, the step playing, and the step being
  edited, readable in the area above the three knobs. Rough layout:

  ```
   1    2    3    4    5    6
  A×4  B×2  A×4  B×4  C×2  STOP
        ▲ playing   [4]
  STEP 4    PART B    ×4
  ```

### Memory

- Tracks no longer reserve 5 minutes each. All 16 tracks' audio and the undo layers come from one pool reserved when
  the plugin loads, as large as everything reserved today (4 x 58 MB of track buffers plus the 400 MB undo pool,
  about 630 MB of address space), with today's fallback to smaller sizes. Only what is recorded becomes resident.
- **Undo Budget keeps its meaning:** it caps what undo layers may take (16-400 MB, default 200 MB), so saved
  pedalboards need no migration. There is no new memory setting. *My call.*
- **When the pool runs low, loops win over undo:** the oldest undo layers are given up first (baked into the loop, as
  the budget does today). When loop audio alone fills the pool, a recording closes as at the 5-minute limit and an
  overdub punches out. The screen shows that memory is full. Nothing is lost silently.
- The 5-minute limit per track stays.

### Clearing, stopping, undo

- Clear Track clears the selected track of the current part. Clear All (hold C while stopped, or the port) clears
  every part, resets L and goes back to part A. The arrangement stays: it is a setting. *My call.*
- Undo and redo act on the selected track of the current part, as today.
- A restart with Song off plays the current part from its top.

## Review focus (failure modes no rule above names)

1. A switch while a long track of the outgoing part is mid-pass: the part only ends when its longest track does.
2. Memory running out during a copy on entry: the copy must not half-happen silently.
3. MOD's start (Sync on) arriving while a switch is queued.
4. Undo right after a switch, on a track that was just copied (one layer, no history).
5. Clear Track emptying the last track of a part, then switching away and back (copy on entry again).

## Left to the builder

- How audio is stored in the pool (for example fixed blocks), and whether a copy shares audio until it changes.
- How the arrangement is persisted (plugin state or hidden ports), checked against DPF's LV2 export and MOD.
- The fade length at a switch, within the existing ramps.
- Widget layouts, within the sketches above.
- The CPU measurement: the existing bench, x86 and Dwarf, part A only against main.

## Later

- **Saving loops with the pedalboard,** so a whole piece (parts, arrangement, loops) loads back. The next design.
  Only recorded audio exists in the pool, which keeps that direct.
