# Design: the Mental Looper face (modgui)

Mockup: `2026-11-25-mental-looper-face-mockup.html`, next to this file. Open it in a browser. The box is the design;
pick **Badge** for the on/off light (the user's choice; the mockup opens on Pedal light). The tiles under the box are
the on/off lights the user turned down. **The mockup wins on any value** (sizes, positions, colours, shapes) that this
text doesn't state.

## Why

The plugin is getting a new name, **Mental Looper**, and its MOD face (the pedal shown on MOD's pedalboard) changes
with it. The snake goes. In its place is a punk face that "goes crazy": a head whose spiral eyes spin with the loop
and whose mouth screams while you record. The face is for the user playing on MOD, in MOD Desktop or the Dwarf's web
interface. At a glance it should show whether the looper is playing, recording or stopped. It should show where the
loop and the selected track are, and which song step is playing. It also has to be fun to look at.

Today's face is a stock MOD wooden box with two film-strip knobs (TRACK, VOLUME) and seven keys
(`plugins/Ouroboros/modgui/`). It shows no state from the plugin except whether a key's own port is on.

## Goals

- The look from the mockup, at 520 × 420 px: an acid-yellow box with pink halftone at the edges, the title in
  ransom-note cut-out letters, and the Mohawk head in the middle.
- The head shows the looper's state (table below). It moves only from what the plugin reports.
- A progress bar for the selected track, filled by the same rule as the bar on the MOD unit's display.
- Controls on the face: REC (the mouth), tracks 1–4, Part A–D, Song, the selected track's Volume, ON/OFF, STOP/PLAY,
  UNDO, REDO, CLEAR, CLEAR ALL, and the 16 song steps (editable, with the playing step lit).
- A press from a footswitch, MIDI or the HMI shows on the face, just as a click does.

## Non-goals

- **Renaming the plugin.** The user will rename it themselves after the current implementation work (the
  footswitches) is done. That covers the name, URI, brand ID, bundle and file names. Until then the face says MENTAL
  LOOPER while MOD's lists still say Ouroboros. Keep today's file names (`icon-ouroboros.html` and so on).
- Changes to the HMI display, the LEDs or the DSP behaviour. The new outputs only report what the engine already
  knows.
- Settings, saved pieces (Slot/Save/Load), Sync, Click and so on on the face. They stay in MOD's settings panel.
- Per-track state lights (REC/PLAY/DUB per track) beyond what the mockup shows.

## Anti-goals (failure even if it works)

- Keys that stick, or presses lost to a short tap. Keep today's momentary logic and its 90 ms minimum hold.
- A face that costs audio: no output work in `run()` beyond copying a few numbers, at a capped rate.
- A pedalboard that gets sluggish with several Mental Loopers on it.
- A stable and a beta copy on one page restyling each other.
- A punk look that makes any control hard to read or hit at 1×.

## Approaches

1. **Chosen: SVG head driven by the face script.** The head is inline SVG in the icon template. The script moves it
   (eye rotation, the mouth path, brows, shake) from port and output changes. The mockup does exactly this, and its
   code (`headSVG`, `mouth`, `spiral`, `hairPath`) is the reference.
2. Pre-rendered animation frames (sprite sheets). Rejected: heavy, and they can't follow the loop position smoothly.
3. Canvas. Rejected: nothing to gain over SVG, and harder to style from the stylesheet.

## The head (decided)

The Mohawk character from the mockup. The head is 220 × 220 at left 150, top 82, in head coordinates −110…110.

- **Head:** a white circle (r 88) with a 4 px black outline.
- **Hair:** six pink spikes with a black outline, between −124° and −56°.
- **Eyes:** the left eye is a large spiral (R 25), turning **once per loop** clockwise. The right eye is a small spiral
  (R 15), turning twice per loop the other way.
- **Brows:** they tilt with the state (below).
- **Mouth:** see below.
- **Scar:** a stitched scar on the right cheek.
- **Label:** "REC" printed on the chin.

The **mouth** is a cavity (pink) with block teeth, the third top tooth missing. Its opening `o` runs from 0 (gritted:
the teeth meet in a 12 px band) to 1 (screaming: 42 px open, upper lip arched, lower lip sagging). The geometry is the
mockup's `mouth(o, 'gap', 6)`.

| Looper state | Eyes | Mouth | Brows / head |
|---|---|---|---|
| Nothing recorded | still | gritted | flat |
| Waiting to record (an Auto Rec arm, or Sync holding a REC press for the bar line) | as for the state it waits in | half open (`o` ≈ 0.36) and trembling (± 0.07, fast) | raised (8 px up), no shake |
| First take (recording, no loop length yet) | spin at a steady rate, one turn per 2 s (*my call*) | screaming | dropped, head shakes |
| Playing | turn with the loop | gritted | asymmetric "crazy" tilt |
| Recording **or overdubbing** (any track) | turn with the loop | screaming (`o` ≈ 0.82–1 with a quick wobble) | dropped, head shakes |
| Stopped | hold where they are | gritted | flat |
| Bypassed (switched off) | X X | flat line, no teeth | faded to half, nothing moves |

The user confirmed that overdubbing screams too. The mouth closes as soon as no track is writing any more.

**Waiting** (*my call*, the user said go): without it the face shows nothing between a REC press and the take, which can
be a bar or more, and that is when you need to see the press landed. The MOD display shows ARM or WAIT then. Recording
wins over waiting: a REC press waiting to close a take still screams.

- **Stopping:** an End of loop or Fade stop in progress counts as playing until the looper has stopped (*my call*).
- **Stopped:** the engine jumps back to position 0 when it stops, so the script keeps the eyes' last angle instead of
  following `progress`.

## The box (decided)

The positions are the mockup's, in px from the box's top left.

- **Box:** 520 × 420, background `#f3e813`, radius 12. Pink (`#ff3d8b`) halftone dots fade in towards the edges.
  Ink is `#111`, the accent `#ff3d8b`, the fill white.
- **Top row:**
  - The on/off light: a **badge** (the user's pick), a punk pin-back button. A pink disc (r 20 in a 48-unit box, 2.5 px
    black outline) tilted −8°, with "ON" in Anton and a white shine arc. Off: the disc turns white and says "OFF".
    The mockup's `powerSVG('badge')` is the reference. It replaces the small head the user didn't like.
  - The brand line "STEFAN", written into the face. The Makefile rewrites `modgui:brand` to the bundle name, so
    `{{brand}}` would show OUROBOROS (*my call*).
  - The title.
  - Fine print top right ("ML-4 / 4 TRACKS").
- **Title:** "MENTAL LOOPER" in cut-out letters: each letter its own scrap with a mixed font, colour, size and tilt,
  and a hard black shadow. The mockup's `ransom()` with seed 11 and the Fluoro palette gives the arrangement. **Bake it
  into one SVG** (letters as outlines, generated once from the fonts by a committed script), so the face ships no
  fonts for it (*my call*).
- **Tracks 1–4:** white circles with a black outline on the head's diagonals. The selected one is black with yellow
  text. A track that is off is faded and struck through.
- **PART:** A–D as white boxes with a 2 px black outline and a hard shadow; the current part is pink. SONG is a black
  key with a dot that turns pink while the song is on.
- **TRACK n:** the Volume knob (black and knurled, white pointer, ticks), its dB value, and ON/OFF below.
- **Keys row** (top 292): STOP/PLAY, UNDO, REDO, CLEAR, CLEAR ALL. Black keys with yellow Anton text; a key is pink
  while pressed or while its port is on.
- **Progress bar** (top 330, 30 → 490, 12 px):
  - White, with a 2 px black outline, filled pink to the selected track's position.
  - A black tick at each loop boundary when the track is longer than one loop.
  - During the first take it shows the elapsed time ("0:07") instead, as the MOD display does (*my call*).
- **Song steps** (top 352): 16 keys in four groups of four. Each shows its part and repeats (A×4), or ↺ / ■ for Loop
  and Stop. The playing step is pink, and steps after the first Loop or Stop are faded. Click a step to cycle its part;
  scroll or drag on it to change the repeats.
  - **Scroll is one repeat per wheel notch.** The user found one step per wheel event far too sensitive (a notch jumped
    1 → 5), because high-resolution and smooth-scrolling wheels send several small events per notch. Add up `deltaY`
    (lines × 40, pages × 800), take a step once the sum reaches 50 px, at most one step per 80 ms, and reset the sum
    after 250 ms of quiet. The mockup's `wheelSteps()` is the reference.
  - **Drag:** one repeat per 8 px.
- **Fonts:**
  - Anton for keys, parts, track numbers and steps.
  - Archivo for the small captions.
  - Bundle both (SIL OFL) with their licence texts.

## MOD mapping (decided)

New **output** ports, before Bypass:

| Symbol (*my call*) | Range | What |
|---|---|---|
| `progress` | 0–1 | The selected track's bar fill, exactly the display's rule (`renderLoopWidget`): track position ÷ track length, or the loop position when the track is empty. |
| `progress_loops` | 1–N | The track's length in loops (1 when empty); gives the ticks and the eyes. |
| `looper` | enum 0–5 | 0 nothing recorded, 1 stopped, 2 playing, 3 recording or overdubbing (any track), 4 first take, 5 waiting to record. 3 and 4 win over 5. |
| `take_secs` | 0–… | Seconds into the first take; 0 otherwise. |
| `song_step` | 0–16 | The playing step, 1–16; 0 when the song isn't playing. |

- **Waiting to record** means an arm (`armedTrack() >= 0`) or a Press or Arm waiting for the bar line. A restart waiting
  for its line (C) and MOD's retrigger don't count. The engine has `waiting()` and `userWaiting()`, but both include
  Restart, so add an accessor for this.
- **Eyes:** the face derives the loop position for the eyes as `frac(progress × progress_loops)`. The track position
  is the global position mod L, so this equals the loop position, and no separate loop-position output is needed.
- **Update rate:** the plugin moves `progress` at most about 30 times a second, and keeps its previous value
  otherwise. The other outputs change only on events.
- **Monitoring:** list all five in `modgui:monitoredOutputs` in `modgui.ttl`. mod-ui forwards an output to the face
  script as a `change` event only when it is listed (`setOutputPortValue`, modgui.js ~846; utils_lilv.cpp ~2578).

**Inputs the face writes or follows.** Each control and the port behind it:

- **Mouth = REC, record only** (*my call*, the user said go). `rec` now records on the press and undoes a layer when
  held for Hold Time (200 ms or more), as footswitch B does. A slow click or a long press on a tablet must not undo, so
  the mouth sends a fixed short press: 1 on pointer down, 0 after 90 ms, whatever the pointer does. The face's UNDO key
  is there for undoing. This is the one exception to "keep today's momentary logic".
- **STOP/PLAY, UNDO, REDO, CLEAR, CLEAR ALL, ON/OFF:** `stop_play`, `undo`, `redo`, `clear_track`, `clear_all`,
  `onoff`, momentary as today. Each lights while held or while its port is on.
- **The Dwarf's footswitches** (after the footswitches work): B sends `rec`, C sends `stop_clear`.
  - The "REC" on the chin turns pink while `rec` is on, whether the press came from a click or a footswitch.
  - STOP/PLAY also lights while `stop_clear` is on (*my call*).
  - The face follows `stop_clear` but writes nothing to it.
- **Tracks:** `track` (1–4) is written on click and followed for the selection. `on_1`…`on_4` give the off look.
- **Parts:** `part` (0–3) is written and followed. The plugin already writes Part back as the song moves
  (`OuroborosPlugin.hpp` ~645), so the lit part follows the song.
- **Song:** `song` toggled.
- **Volume:**
  - The knob is bound to `volume`, the selected track's proxy (−60…6 dB), through `mod-role=input-control-port` with
    a fully transparent film strip, so mod-ui still handles dragging.
  - The knob is drawn in CSS from a `--v` the script sets, the Boreas way.
- **Steps:** the face writes the hidden `stepN_part` / `stepN_repeats` ports directly with `funcs.set_port_value`.
  - The ports are writable from the face: `setPortValue` uses `self.controls`, built over every input port, including
    hidden ones.
  - The plugin already keeps the Step proxies in step (~495–509).
  - The face follows their `change` events, so a loaded piece or preset shows up.
- **Bypass:**
  - The badge lives inside `[mod-role=bypass-light]`, styled from mod-ui's `.on` / `.off`, with a transparent
    `[mod-role=bypass]` on top.
  - The big head's off state follows the `:bypass` change event (mod-ui sends it, modgui.js ~616). Read the
    bypass-light's class at `start` for the initial state.

## MOD lessons from Boreas and Sitar (must)

See `~/Fun/boreas/docs/superpowers/specs/2026-11-08-ice-chunks-modgui-design.md`, "MOD lessons from Sitar" and "Build
notes from review".

1. Every `font-family` with `!important`, with plugin-unique family names.
2. Scope every selector to the root class plus `{{{cns}}}`, prefix `@keyframes`, and use **new** class names, so the
   old stylesheet can't touch the new face.
3. `box-sizing: border-box` on the pedal and everything in it.
4. Footswitch `z-index` set explicitly.
5. `/resources/<path>{{{ns}}}` URLs. The Makefile `modgui:` target copies the new folders (fonts, the title SVG);
   today it copies only html, css, js, png and `knobs/`.
6. `mod-no-shadow` on the first child.
7. `render_screenshot.py`:
   - mirrors mod-ui's `*` rules and rewrites `/resources/` URLs;
   - waits for fonts;
   - sets the knob's `--v`;
   - poses the head in a fixed frame. *My call:* screaming, track 1, the bar at about 40 %, the song on step 1. The
     screenshot and the thumbnail both come from this pose.
8. A hook test like `~/Fun/sitar/tests/test_modgui_hooks.py`. It checks:
   - the face's port symbols;
   - the required `mod-role`s;
   - scoping, `!important` fonts and the footswitch z-index;
   - that the five outputs are listed in `monitoredOutputs`.

   Ouroboros runs C++ tests with `make test`, so add a Python step or target and say how it's run.

## Also to update

- `screenshot-ouroboros.png` and `thumbnail-ouroboros.png` (required, or MOD Desktop's scanner crashes).
- The manual: it shows the face screenshot (`docs/manual/ouroboros-manual.html` ~293). Refresh the image and any
  control-map pins, and add a line saying what the head shows (eyes = loop, mouth = recording, bar = track). Then
  `make manual`.
- The beta build: its `modgui:label` is "Ouroboros (Beta)". The face's fine print shows **BETA** in the beta build so
  the two can be told apart side by side (*my call*). How is up to the builder, for example `{{label}}` or a class
  from the Makefile's sed.

## Sequencing

The footswitches work has landed (`c3a0c90` to `28c24cb`): `rec` is momentary Rec/Undo, `undo_redo` became
`stop_clear`, and both get `mod:preferMomentaryOnByDefault` from the Makefile. Plan against the code as it is now.

## Verification

1. `make`, `make test` and the hook test pass. A plugin test covers the outputs: the bar rule against
   `renderLoopWidget`'s for an empty, a 1-loop and a 4-loop track and the first take; `looper` for each state,
   including an overdub on a non-selected track, an Auto Rec arm, a Sync-held REC press, and a C restart waiting for
   its line (not 5); `song_step`.
2. In MOD Desktop, with beta and stable on one page:
   - The face matches the mockup.
   - The eyes turn once per loop. The bar fills and ticks correctly when switching tracks.
   - The mouth screams on REC and on DUB, and closes after. With Auto Rec or Sync on it trembles half open first.
   - Holding the mouth for a second records and does not undo.
   - The step key follows the song. Editing a step changes the song (check against the HMI's song page).
   - Presses from a footswitch or MIDI light the keys.
   - Bypass makes the face go X-eyed.
   - The two copies don't restyle each other.
3. Four instances on one pedalboard stay responsive.
4. The Dwarf: an existing pedalboard still loads.

## Left to the builder

- Output encoding details (port hints, `notOnGUI` or not), and how the 30 Hz cap is timed.
- How the script schedules the head:
  - nothing runs while the head doesn't move (stopped, bypassed, nothing recorded);
  - the scream wobble and the steady first-take spin may use a short animation loop or CSS.
- The tool for baking the title SVG, and the knob sprite.
- Pixel nudges after the on-device check.

## Calls I made (not asked for)

- Waiting to record shows on the face (mouth half open and trembling, brows up), as `looper` = 5.
- The mouth records only: a fixed 90 ms press, so holding it never undoes.
- During the first take the eyes spin at a steady rate (one turn per 2 s) and the bar shows the elapsed time.
- The eyes come from `progress × progress_loops`; there is no separate loop-position output.
- Output symbols and the `looper` encoding.
- The title is baked into an SVG instead of shipping seven fonts.
- The screenshot pose is the scream.
- BETA in the fine print for the beta build, from `{{label}}`. STEFAN is written into the face.
- An End of loop or Fade stop counts as playing until the looper has stopped.
- The chin's "REC" lights while `rec` is on. STOP/PLAY also lights for footswitch C (`stop_clear`).
- File names stay `*-ouroboros.*` until the user's rename.
