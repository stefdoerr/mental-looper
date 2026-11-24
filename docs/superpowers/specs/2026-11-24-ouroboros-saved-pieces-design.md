# Saved pieces (design)

Status: draft for approval, 2026-11-24. Decisions marked *my call* were proposed by me and not argued; they are open to
correction.

## Why

Song parts made it possible to build a whole piece on the Dwarf: four parts, an arrangement, up to 16 tracks. All of it
is gone when the pedalboard is loaded again, and a pedalboard holds only one arrangement. The user wants a library of
whole pieces inside one pedalboard, so a live set can go piece 1, piece 2, piece 3 without re-recording anything.

Success: build a piece, save it to a slot, build another, save it to the next; later, on the Dwarf alone, load either
one between songs and play it with its arrangement, exactly as it sounded when saved.

## Goals

- **Numbered slots, 1 to 16,** each holding one whole piece (*my call:* 16).
- **A piece is everything you hear:** all 4 parts with their 4 tracks, each track's audio as heard, volume and on/off,
  the loop length L and the bars, the arrangement and Song on/off, the current part.
- **Save works at any time,** even while the loops play, without a gap or a glitch.
- **Load works between songs:** it stops the loops, reads the piece, and leaves it ready to start.
- **Everything happens on the Dwarf,** on a second control page, with a widget that shows the slots.
- **The pedalboard remembers its piece:** loading the pedalboard loads the piece last saved or loaded with it.
- **Pieces are ordinary files** the user can see, copy to a computer and back up with MOD's file manager.

## Non-goals

- Loading a piece while another plays, with a seamless segue (considered and set aside: needs memory for two pieces).
- Loading a single loop into a track, or mixing loops from different pieces (the file format must not rule it out).
- Names for pieces, or renaming and deleting them from the Dwarf (the file manager can delete a slot's folder).
- Saving undo and redo history.
- Resampling a piece saved at another sample rate.

## Anti-goals

- **A glitch or gap in the audio** because Save runs, or any wait on the audio thread for the disk.
- **Losing a piece:** a Save that fails half-way must leave the slot's old piece intact, and a Save or Load that would
  throw away work needs a second press.
- **CPU growth for someone who never saves:** playing costs what it costs today (the CPU rule).

## Constraints

- Real-time safe: no file I/O, allocation, locks or unbounded work in `run()`. All file work runs on a background thread
  owned by the plugin (DPF has `distrho/extra/Thread.hpp`).
- The block pool and its refcounts belong to the audio thread today (single-threaded engine).
- The Dwarf: 5.5 GB free on `/data`; MOD's user files live in `/data/user-files`, which already has an `Audio Loops`
  folder. About 767 MB of RAM is free with a pedalboard loaded, no swap.
- Dwarf controls (from the firmware): footswitch A changes page; a page change swaps knobs and footswitches together.
  Page 1 is full: subpage 1 Track, On/Off, Volume; subpage 2 Part, Song, one free knob; subpage 3 Step, Step Part,
  Step Repeats. A parameter can be assigned to one actuator only.
- Saved pedalboards connect by symbol: existing symbols are frozen, new controls go before Bypass. Hidden ports are
  saved and restored with the pedalboard (traced through mod-host and mod-ui for the song steps).
- No LV2 state: MOD calls state save only when the pedalboard is saved, never on a Save press, and the plugin's
  parameter mapping assumes no state port. DPF changes stay in the local patch.

## Approaches considered

1. **The plugin writes and reads files itself, on a background thread (chosen).** Save and Load are buttons, so the
   plugin must act on a press; files in user files make pieces visible and reusable across pedalboards.
2. **LV2 state, files in the pedalboard's folder.** MOD triggers it only on pedalboard save, so it cannot give several
   slots in one pedalboard. Rejected.
3. **Named files picked from a list.** The Dwarf has no keyboard; picking a file needs the web UI or firmware support
   for file lists. The user chose numbered slots.

## Behaviour

### Slots and what they hold

- A slot is a folder: `/data/user-files/Audio Loops/Ouroboros/01` to `16` (*my call*). In it, one mono WAV per
  non-empty track (32-bit float, the session's sample rate; for example `B3.wav` for part B, track 3) and a small text
  file with the rest: sample rate, L, bars, the BPM if Sync knew one, each track's length, volume and on/off, the
  current part, Song on/off and the 16 steps. *My call:* 32-bit float keeps the audio exact and opens in any DAW.
- A track's WAV holds what that track plays over its whole length, every layer still in, starting at its beginning.

### Saving

- **Save** writes the piece to the selected slot. It works while stopped or playing. The audio keeps playing untouched:
  the plugin takes an instant snapshot (the tracks already share audio until written, as copy on entry does) and a
  background thread writes it.
- A take running at the press (a recording or an overdub) is not split: the save waits until it closes, then takes the
  snapshot. *My call.*
- **Overwriting:** Save on a slot that holds a piece asks first: the widget shows `SAVE 3?`, and a second Save within
  about 2 s confirms; anything else cancels. Save on an empty slot writes at once.
- **Nothing half-written:** the piece is written beside the slot and swapped in only when complete. If the disk fills or
  a write fails, the slot keeps its old piece and the widget shows the failure.
- A second Save while one is still writing is ignored. *My call.*
- Saving an empty piece (nothing recorded) does nothing. *My call:* it would only overwrite a slot with silence.

### Loading

- **Load** loads the selected slot. If the loops are playing it stops them at once (Instant), then reads the piece.
  While it reads, the widget shows the progress and B, C, Part and Song wait. *My call:* presses during a load are
  ignored, not queued.
- **Throwing away work asks first:** if the current piece has changes since it was last saved or loaded, the widget
  shows `LOAD 3?` and a second Load within about 2 s confirms.
- Load of an empty slot does nothing (the widget says so).
- After a load: all tracks stopped at their beginning, the saved volumes and on/off, the saved part and arrangement,
  Song as saved. B, or Stop/Play, starts it; with Song on it plays the arrangement from step 1. Each track has one
  layer, no undo history.
- **A broken piece** (missing or unreadable files, another sample rate, a track longer than the 5-minute limit) is
  checked before anything is cleared where possible; the current piece then stays. A read error after the clear leaves
  every track empty and the widget shows the error. *My call.*
- **A piece too big for memory** can happen: tracks that shared audio when saved (a part copied on entry) are read back
  as separate audio, and the pool may have been reserved smaller (its fallback when RAM is short). Load checks the
  piece's total size against the pool before the clear; if it does not fit, the current piece stays and the widget
  shows `TOO BIG`. *My call:* re-sharing identical audio on load is left out; the check is the guarantee. (The Undo
  Budget does not matter here: it caps undo layers only, and a loaded piece has none.)
- **Controls follow the piece:** after a load, the Part, Song, step, On 1-4 and Volume 1-4 controls show the piece's
  values, so saving the pedalboard afterwards keeps them. Length stays as it is: it only shapes a first recording, and
  a loaded piece already has its L. *My call.*

### The pedalboard

- The plugin remembers the last slot saved or loaded in a hidden port, saved with the pedalboard. Loading the
  pedalboard loads that slot's piece as a Load would, and sets the Slot control to it. A pedalboard saved with no piece
  loads none. *My call.*
- Saving a piece does not save the pedalboard; the pedalboard's remembered slot updates only when the pedalboard is
  saved, as any control does.
- If the remembered slot was overwritten from another pedalboard since, the newer piece loads.
- If the remembered piece cannot load (gone, broken, another sample rate, too big), the pedalboard comes up with no
  piece and the pieces widget shows why.

### Tempo

- With Sync on, a piece stores the BPM it was recorded at. After a load the widget shows it, so MOD's tempo can be set
  to match; a plugin cannot set MOD's tempo. Loops keep their length, as today, so a different tempo drifts.

### Controls and screen

- **Page 2 (pieces):** knob 1 Slot (1 to 16), knob 2 Save (press), knob 3 Load (press), and the pieces widget above
  them. Footswitches B and C are free on that page; the user may assign Save and Load there instead.
- **Pieces widget:** the 16 slots, marking the ones that hold a piece, the selected one and the loaded one; a status
  line for `SAVE 3?`, `LOAD 3?`, `SAVING`, `LOADING 40%`, `SAVED 3`, `EMPTY`, `DISK FULL`, `TOO BIG`, `LOAD ERROR`, and after a
  load the piece's BPM and bars. Rough layout:

  ```
   1  2  3  4  5  6  7  8
   ■  ■  □  ■  □  □  □  □
   9 10 11 12 13 14 15 16
   □  □  □  □  □  □  □  □
  [3] EMPTY      loaded 2
  ```

- The loop widget shows `LOAD` while a load runs. *My call.*

## Review focus (failure modes no rule above names)

1. Save while an overdub runs on a track that shares blocks with the snapshot (memory for the copies, the snapshot
   staying exact).
2. The pool filling during a Save's snapshot: loops still win; the save fails cleanly, never the audio.
3. Loading a pedalboard whose remembered slot is gone, broken or from another sample rate.
4. Power loss or a pedalboard change during a Save.
5. Clear All, Undo or a part switch while a Save is still writing.

## Left to the builder

- How the background thread and the audio thread hand over the snapshot and the loaded audio without locks, given that
  the pool and its refcounts belong to the audio thread.
- The text file's format, and finding the user files folder on hosts other than the Dwarf.
- How "changed since saved" is tracked.
- The widget layout, within the sketch.
- The CPU check: the existing bench, x86 and the Dwarf at the plugin's real flags, against main.

## Later

- Loading single loops into a track; a seamless segue between pieces.
