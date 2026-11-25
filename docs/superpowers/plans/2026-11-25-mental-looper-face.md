# Mental Looper Face Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the stock MOD wooden-box face with the Mental Looper punk face: a Fluoro box, a cut-out title and the
Mohawk head. The head's eyes turn with the loop, and its mouth screams while recording and trembles while a REC waits.
It has a progress bar, the 16 song steps and a pin-back badge for on/off. Five new plugin outputs drive it.

**Architecture:**
- The plugin works out five numbers each block from the engine (`hmi/FaceView.hpp`, a pure function) and publishes them
  as hidden LV2 output ports. `modgui.ttl` lists them as monitored outputs.
- The face is a static template with every shape baked in (`generate_face.py` writes the head SVG into the template and
  the title into `face/title.svg`).
- The face script only toggles classes, sets CSS variables, rotates the eyes and writes ports. Every animation is a CSS
  transform.

**Tech Stack:** C++14 / DPF (LV2), MOD modgui (HTML, CSS, a mod-ui JS hook), Python 3 (fontTools for baking, Playwright
with the system Chrome for the face test and the screenshot), the repo's test harness.

**Spec:** `docs/superpowers/specs/2026-11-25-mental-looper-face-design.md` (approved), with the mockup
`docs/superpowers/specs/2026-11-25-mental-looper-face-mockup.html` next to it. Read both before any task. Pick **Badge**
in the mockup's "On/off light" bar. The mockup wins on any pixel value this plan doesn't state.

## Global Constraints

- **No rename.** The face says MENTAL LOOPER and STEFAN. File names stay `*-ouroboros.*`, and the URI, brand and bundle
  are unchanged. The user renames later.
- **Ports:** the five outputs go after `piece` and before Bypass: `progress`, `progress_loops`, `looper`, `take_secs`,
  `song_step`. Bypass moves to index 80, and there are 81 parameters in all. No other port moves.
- **`looper` values:** 0 nothing recorded, 1 stopped, 2 playing (a Stopping fade counts as playing), 3 recording or
  overdubbing on any track, 4 first take, 5 waiting to record. Precedence is 4, then 3, then 5, then 0, then 1, then 2.
- **Box:** 520 × 420 px, background `#f3e813`, radius 12. Ink `#111`, accent `#ff3d8b`, fill `#fff`, key text
  `#f6ec1a`.
- **Fonts:** Anton and Archivo, bundled as woff2 under the family names `MentalLooperAnton` and `MentalLooperArchivo`.
  Every `font-family` ends in `!important`.
- **Styles:** every selector starts with `.ml-pedal{{{cns}}}`. `@keyframes` names start with `ml` and carry `{{{cns}}}`.
  Only `transform` and `opacity` animate. All new class names start with `ml-`, and none of the old `ouroboros-*` ones
  are left.
- **The template has no `id=` attributes** (two pedals on a page would share them), so no `clipPath` or gradients.
- **The face writes ports** only through `funcs.set_port_value`. A key holds its 1 for at least 90 ms. The mouth sends 1,
  then 0 after 90 ms, whatever the pointer does.
- `make test`, `make test-lv2` and (from Task 4) `make test-modgui` pass at the end of every task.
- Commit subjects look like `plugin: …`, `modgui: …` or `docs: …`, and every commit ends with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Rulings made while planning

- **Bar rule shared, not copied.** The widget's choice of length and position moves into `barSource()` in
  `hmi/LoopWidget.hpp`. `renderLoopWidget` and `faceView` both call it, so the face's bar can't drift from the display's.
- **Outputs are hidden** (`kParameterIsHidden`, which becomes `notOnGUI`) and integer except `progress`. If MOD Desktop
  stops forwarding them to the face, drop the hidden flag (Task 6 checks this).
- **Update rate:** `progress` moves at most once per `sampleRate / 30` samples. `take_secs` is whole seconds. The others
  are written every block, and the host only sends changes.
- **No animation loop in JS.**
  - The four mouths (gritted o = 0, waiting o = 0.36, screaming o = 0.91, dead) are baked into the template as separate
    groups, and CSS shows one of them.
  - The scream wobble (`scaleY` 0.93 ↔ 1.07, 0.28 s) and the waiting tremble (`scaleY` 0.91 ↔ 1.09, 0.12 s) are CSS
    keyframes, alternating.
  - During the first take the eyes spin freely in CSS: the big eye one turn per 2 s clockwise, the small one 1 s
    anticlockwise.
  - The script only sets the eyes' `rotate()` when `progress` changes.
- **The script uses the native DOM** (`event.icon[0]`, `querySelector`, `addEventListener`) and keeps its state on the
  element. This is what lets a Playwright test drive the real script against the real template.
- **BETA:** for `BETA=1`, the Makefile's `modgui` target seds the copied icon from `ML-4<br>` to `ML-4 BETA<br>`. It's
  static, so it shows without JS too.
- **Title bake:**
  - Fonts are fetched from the google/fonts GitHub repo into `build/face-fonts/` and are not committed.
  - Each letter scrap is laid out from the font's advance widths, not from a browser, so it's close to the mockup, not
    pixel-equal. The on-device check covers nudges.
- **Knob strip:** copy `~/Fun/boreas/plugins/Boreas/modgui/knobs/clear.png` (a transparent 65-frame strip of 64 px
  frames) to `knobs/clear.png`, and delete `knobs/black.png`.
- **Eyes while waiting (5)** follow `progress`, as when playing. An arm while stopped therefore shows the eyes at the
  stopped position.

## Review Focus

1. **Bypassed while recording.** `looper` = 3 and `:bypass` = 1 together show the dead face: X eyes, a flat mouth, no
   shake and no wobble. (Task 5, `test_bypass_wins_over_state`)
2. **Switching from a 4-loop track to a 1-loop track.** The bar's ticks disappear, and the new fill shows.
   (Task 5, `test_ticks_follow_track_switch`)
3. **A key pressed and dragged off before release.** The key still sends its 0 and unlights. (Task 5,
   `test_key_released_on_leave`)
4. **Scrolling past the ends.** Repeats stop at 16 and at 1, with no write. Scrolling on a Loop or Stop step writes
   nothing. (Task 5, `test_step_repeats_clamp`)
5. **The face before any output arrives** (a fresh pedal, or mod-ui's drag-in preview where no script runs). It shows
   the nothing-recorded pose with an empty bar, track 1, part A, 0 dB, and the default song (A×1 then Loop).
   (Task 4, `test_static_defaults`)

---

### Task 1: The face's numbers (engine accessor, shared bar rule, `faceView`)

**Files:**
- Modify: `plugins/Ouroboros/dsp/Looper.hpp`. Add an accessor next to `userWaiting()` (~line 612).
- Modify: `plugins/Ouroboros/hmi/LoopWidget.hpp`. Add `barSource()`, and use it in `renderLoopWidget` (~lines 112–117).
- Create: `plugins/Ouroboros/hmi/FaceView.hpp`
- Create: `tests/test_face.cpp` (engine tests on `tests/looper_rig.hpp`'s `Rig`, in the style of
  `tests/test_longtracks.cpp`)

**Interfaces:**
- Produces:
  - `bool Looper::recWaiting() const`: `armed_ >= 0 || pend_.kind == Pending::Press || pend_.kind == Pending::Arm`.
  - `struct BarSource { uint64_t len, pos; };` and
    `inline BarSource barSource(TrackState selState, uint32_t trackLength, uint32_t trackPosition, uint32_t loopLength, uint32_t position)`.
    It returns the track's own length and position when `selState != Empty && trackLength > 0`, otherwise the loop's
    (`len` is 0 when there is no loop).
  - `enum FaceLooper : int { kFaceEmpty = 0, kFaceStopped, kFacePlaying, kFaceWriting, kFaceFirstTake, kFaceWaiting };`
  - `struct FaceView { float progress; int loops; int looper; int takeSecs; int songStep; };`
  - `inline FaceView faceView(const Looper& e, int sel)`, defined as follows:
    - `progress`: `pos / len` from `barSource` (0 when `len == 0`).
    - `loops`: `max(1, trackLen / loopLength)`, where `trackLen` is 0 for an empty selected track. It is 1 with no loop.
    - `takeSecs`: `recordedSamples() / sampleRate()`, floored, while `loopLength() == 0` and any track is Recording.
      Otherwise 0.
    - `songStep`: `songStep() + 1`.
    - `looper`: in the precedence order from Global Constraints.

- [ ] **Step 1: Write the failing tests** in `tests/test_face.cpp` (`static const uint32_t L = 24000;`):
  - `emptyTrackFollowsTheLoop`: `recordBase(r, L, 0.1f)` on track 0, then `r.silence(7000)`. For `sel = 1`:
    `progress == float(r.lp.position()) / L` and `loops == 1`.
  - `oneLoopTrack`: as above, but for `sel = 0`: `progress == float(r.lp.trackPosition(0)) / r.lp.trackLength(0)` and
    `loops == 1`.
  - `fourLoopTrack`:
    - Base on track 0, then go to phase 0 (`toPos`, as in `test_longtracks.cpp`).
    - `r.lp.pressRec(1); r.feed(sine(220, 0.1f, 4 * L)); r.lp.pressRec(1); r.silence(5000);`
    - Check `r.lp.trackLength(1) == 4 * L`, `faceView(r.lp, 1).loops == 4`, and
      `progress == float(r.lp.trackPosition(1)) / (4 * L)`.
  - `firstTakeShowsSeconds`:
    - `r.lp.pressRec(0); r.feed(sine(220, 0.1f, 72000));` (1.5 s): `looper == kFaceFirstTake`, `takeSecs == 1`,
      `progress == 0`, `loops == 1`.
    - After `pressRec(0)` closes the take, `takeSecs == 0`.
  - `widgetBarUnchanged`: `tests/test_widget.cpp` must still pass unchanged, which guards the `barSource` refactor. No
    new code goes in this test file for it.

- [ ] **Step 2: Run, verify it fails.** Run `make test`. Expected: `tests/test_face.cpp` fails to compile
  (`FaceView.hpp` not found).

- [ ] **Step 3: Implement** `recWaiting()`, `barSource()` (make `renderLoopWidget` call it for `len` and `pos`, nothing
  else changes) and `FaceView.hpp` (it includes `LoopWidget.hpp`).

- [ ] **Step 4: Run `make test`.** Expected: `==> all tests passed`, with `test_face` and `test_widget` among them.

- [ ] **Step 5: Commit**: `engine, hmi: the face's numbers from the engine; the bar rule shared with the widget`.

### Task 2: Five output ports

**Files:**
- Modify: `plugins/Ouroboros/OuroborosPlugin.hpp`:
  - The enum: add the five after `kParamPiece`.
  - `initParameter`: outputs, before the `defs` table lookup.
  - `activate()`: reset the rate counter.
  - The end of `run()`'s main path: after `fHmi.update(...)`.
  - A new member `uint32_t fFaceAcc`.
- Modify: `tests/test_plugin.cpp`: `layout()`, plus new tests.
- Modify: `tests/lv2/test_lv2_hooks.cpp`: one new test.

**Interfaces:**
- Consumes: `ouro::faceView(const Looper&, int)` (Task 1).
- Produces: `kParamProgress, kParamProgressLoops, kParamLooper, kParamTakeSecs, kParamSongStep` (indices 75–79), with
  the following port definitions:

  | Symbol | Name | Range | Default | Type |
  |---|---|---|---|---|
  | `progress` | Progress | 0–1 | 0 | float |
  | `progress_loops` | Progress Loops | 1–4096 | 1 | integer |
  | `looper` | Looper | 0–5 | 0 | integer |
  | `take_secs` | Take Seconds | 0–599 | 0 | integer |
  | `song_step` | Song Step | 0–16 | 0 | integer |

  All five have the hints `kParameterIsOutput | kParameterIsHidden` (plus `kParameterIsInteger` on the integer ones)
  and no group.

- [ ] **Step 1: Write the failing tests** in `tests/test_plugin.cpp`:
  - `layout()`:
    - Symbols 75–79 are as above. Each has `kParameterIsOutput` and `kParameterIsHidden`.
    - `kParamBypass == 80 && P().getParameterCount() == 81`. This replaces the 75/76 check.
  - `faceLooperFollowsTheEngine`:
    - `fresh()`, `runN(2)`: looper 0.
    - Press `rec` (`set(kParamRec,1); runN(1); set(kParamRec,0); runN(20)`): looper 4, and
      `P().getParameterValue(kParamTakeSecs) == 0`.
    - `runN(400)`: take_secs 1.
    - Press `rec`, `runN(40)`: looper 2.
    - `set(kParamTrack, 2)`, press `rec`, `runN(5)`: looper 3. Then `set(kParamTrack, 1); runN(2)`: still 3 (a
      non-selected track writing).
    - Press `rec` with track 2 selected again to close it, then press `stop_play` and `runN(10)`: looper 1.
  - `faceShowsAnAutoRecArm`: `fresh(); set(kParamAutoRec, 1); set(kParamThreshold, -10); runN(1);`, then press `rec`:
    `eng().armedTrack() == 0` and looper 5.
  - `faceShowsASyncHeldPress`: the setup of `syncPressWaitsForTheBar` up to its first `CHECK`: looper 5. After
    `runMod(true, 358)`: looper 4.
  - `faceFirstTakeWinsOverAWaitingClose`: `waitingClose()`: `eng().userWaiting(0)` and looper 4.
  - `faceIgnoresARestartWaitingForItsLine`:
    - Sync on, with a one-bar loop as in `modPlayAndStopEdgesFollow`.
    - Stop with a double-tap of C (`doubleTap()` with `runMod` timing, or `stop_play`), and wait until
      `eng().transport() == Stopped`.
    - Tap C mid-bar. `eng().waiting() && eng().waitingTrack() == -1` holds, and looper is 1, not 5.
    - If a stopped C tap doesn't queue a restart under Sync in the code as it is, say so in the report and pin
      whichever wait `userWaiting()` covers but `recWaiting()` doesn't.
  - `faceProgressMovesAtMost30TimesASecond`:
    - After a base (the first half of `recordBaseAndDub()`), run 375 single blocks.
    - Count the changes of `getParameterValue(kParamProgress)`: between 29 and 31.
    - The last value is within `1600.0f / eng().loopLength()` of `float(eng().trackPosition(0)) / eng().trackLength(0)`.
  - `faceSongStep`:
    - `song({{0, 2}, {1, 1}})` (the existing helper), `set(kParamSong, 1)`, and a recorded loop.
    - `song_step` reads 1, then 2 after two passes. With Song off it reads 0.
  - In `tests/lv2/test_lv2_hooks.cpp`, `faceOutputsReachTheHost`:
    - Find `looper` and `progress` with `portIndex(ttlPath, ...)`, and record as `widgetRedrawsWhenProgressMoves` does
      with `gPorts[rec]`.
    - During the take `gPorts[looper] == 4`. After the close `gPorts[looper] == 2`, and `gPorts[progress]` differs
      between two runs 2000 samples apart.

- [ ] **Step 2: Run, verify it fails.** Run `make test`. Expected: compile errors (`kParamProgress` undeclared).

- [ ] **Step 3: Implement.**
  - In `initParameter`, an early branch for indices `kParamProgress..kParamSongStep` sets symbol, name, ranges and hints
    as in the table.
  - In `run()`, `updateFace(frames)` writes `fVal[...]` from `faceView(fEngine, fSel)`. `progress` is only written when
    `fFaceAcc >= getSampleRate() / 30`, which then resets `fFaceAcc`.
  - A load's early-return path writes nothing (the last values stand; the engine is lent).

- [ ] **Step 4: Run `make test` and `make test-lv2`.** Expected: both pass. In the generated TTL
  (`bin/ouroboros.lv2/ouroboros.ttl`), `progress` is an `lv2:OutputPort` with the `notOnGUI` property.

- [ ] **Step 5: Commit**: `plugin: five face outputs (progress, loops, looper state, take seconds, song step)`.

### Task 3: Face assets (fonts, knob strip, baked title) and their bundling

**Files:**
- Create: `plugins/Ouroboros/modgui/generate_face.py`. This task writes its `fonts` and `title` parts; Task 4 adds
  `head`.
- Create, all generated and committed:
  - `plugins/Ouroboros/modgui/fonts/anton-400.woff2` and `OFL-Anton.txt`
  - `fonts/archivo-var.woff2` (the wdth and wght axes kept) and `OFL-Archivo.txt`
  - `plugins/Ouroboros/modgui/face/title.svg`
- Create: `plugins/Ouroboros/modgui/knobs/clear.png` (copied). Delete: `knobs/black.png`.
- Modify: `Makefile`:
  - The `modgui` target copies the `fonts`, `face` and `knobs` dirs (the Boreas loop:
    `for d in knobs fonts face; do …`).
  - The beta sed on the copied icon.
  - A new `face-assets` target: `uv run --quiet --with fonttools --with brotli python3 plugins/Ouroboros/modgui/generate_face.py`.

**Interfaces:**
- Produces:
  - `/resources/fonts/anton-400.woff2` and `/resources/fonts/archivo-var.woff2`.
  - `/resources/face/title.svg`: viewBox `0 0 520 48`. It is placed by Task 4 at left 0, top 23, so the letter row's
    centre falls at y ≈ 47, as in the mockup.
  - `generate_face.py` has `fonts()`, `title()` and (Task 4) `head(template_path)`. It runs all three in order with no
    arguments.

- [ ] **Step 1: Write the self-check** at the end of `generate_face.py` (it runs after generating):
  - `title.svg` parses as XML and holds 12 letter groups (`MENTALLOOPER`).
  - It contains no `<text>`, no `font-family` and no `id=`.
  - Its letters use only the colours of the Fluoro LOOKS from the mockup's `titleHTML(…, rn=true)`, plus `#111`.
  - Both woff2 files start with `wOF2`.

- [ ] **Step 2: Implement `fonts()`.**
  - Download `ofl/anton/Anton-Regular.ttf` and `ofl/archivo/Archivo[wdth,wght].ttf`, with their `OFL.txt`, from
    `https://github.com/google/fonts/raw/main/` into `build/face-fonts/`.
  - Subset to U+0020–007E, U+00D7, U+2212, U+00B7 and write woff2 (`fontTools.subset`, `flavor="woff2"`).
  - ↺ and ■ fall back to a system font; that's fine.

- [ ] **Step 3: Implement `title()`.**
  - Port the mockup's `ransom()` exactly:
    - seed 11, `seed = seed * 16807 % 2147483647`;
    - per letter: the font index (re-drawn while equal to the last), the look, `size = 15 + round(rnd * 6)` (JS round
      = `floor(x + .5)`), `rot = rnd * 14 - 7` and `dy = rnd * 4 - 2`;
    - spaces draw nothing and take 8 px.
  - The fonts, in the mockup's order:
    - Anton
    - Abril Fatface
    - Courier Prime Bold
    - Bungee
    - Playfair Display at wght 900 (`varLib.instancer`)
    - Rubik Mono One
    - Special Elite (`apache/specialelite/`)
  - Each scrap is a rect `advance × size / upm + 6` wide and `size + 3` tall, with 2 px between scraps.
    - The row is centred at x = 260, and each scrap's centre is at y = 24 + dy.
    - The glyph outline (`SVGPathPen`, y flipped) is centred in the scrap.
    - A `#111` copy of the rect sits behind it, offset 1.5, 1.5.
    - The whole group is rotated `rot` degrees about the scrap's centre.

- [ ] **Step 4: Run `make face-assets`.** Expected: the self-check prints `ok`. Open `face/title.svg` in Chrome next to
  the mockup's title, and the letters, fonts and colours match letter for letter.

- [ ] **Step 5: Wire the Makefile.**
  - `make modgui` copies `fonts/`, `face/` and `knobs/` into `bin/ouroboros.lv2/modgui/`.
  - `make BETA=1 modgui && grep -c 'ML-4 BETA' bin/ouroboros-beta.lv2/modgui/icon-ouroboros.html` prints 1 once Task 4
    puts `ML-4<br>` in the icon. Until then, check that the sed line is in the target.

- [ ] **Step 6: Run `make test` and `make test-lv2`** (still passing). Then **commit**:
  `modgui: bundled fonts, the baked cut-out title, a clear knob strip`.

### Task 4: The face (template, baked head, stylesheet, ttl) and its static checks

**Files:**
- Rewrite: `plugins/Ouroboros/modgui/icon-ouroboros.html` and `stylesheet-ouroboros.css`.
- Modify: `generate_face.py`. Add `head()`, which writes between `<!-- ml-head:begin -->` and `<!-- ml-head:end -->` in
  the icon.
- Modify: `plugins/Ouroboros/modgui.ttl`.
- Modify: `plugins/Ouroboros/modgui/render_screenshot.py`. Expose `page_html()` and `launch()`, and the pose comes in
  Task 5.
- Create: `tests/test_modgui_hooks.py` (static, plain `python3`).
- Modify: `Makefile`. Add `test-modgui: python3 tests/test_modgui_hooks.py` (Task 5 adds the browser test to it).

**Interfaces:**
- Consumes: Task 3's `/resources/...` files.
- Produces: the DOM the script and tests use. All elements are inside the root
  `<div class="mod-pedal ml-pedal{{{cns}}} mod-no-shadow ml-st-0">`:

  | Element | What |
  |---|---|
  | `.mod-drag-handle[mod-role=drag-handle]` | |
  | `.mod-light[mod-role=bypass-light]` | contains `svg.ml-badge` with `.ml-badge-on` / `.ml-badge-off` (the mockup's `powerSVG('badge')` without ids) |
  | `.mod-footswitch[mod-role=bypass]` | a transparent hit area at left 26, top 22, 46 × 46 |
  | `.ml-brand`, `.ml-fine` | "STEFAN"; `ML-4<br>4 TRACKS` |
  | `img.ml-title` | `src="/resources/face/title.svg{{{ns}}}"` |
  | `svg.ml-head` | baked; viewBox −110 −110 220 220; at left 150, top 82 |
  | `button.ml-rec` | the mouth hit area: left 208, top 202, 104 × 82 |
  | `button.ml-trk[data-track=1..4]` | |
  | `button.ml-pt[data-part=0..3]`, `button.ml-song` | |
  | `.ml-tn` | the track number in "TRACK n" |
  | `.ml-knob` | sets `--v`, and holds `.mod-knob-image[mod-role=input-control-port][mod-port-symbol=volume]` |
  | `.ml-db` | the dB text |
  | `button.ml-key[data-port=…]` | onoff (in the track box); stop_play, undo, redo, clear_track, clear_all (in the keys row, labelled STOP/PLAY, UNDO, REDO, CLEAR, CLEAR ALL) |
  | `.ml-prog` | holds `.ml-prog-fill` (width `calc(var(--p) * 100%)`), `.ml-prog-ticks` and `.ml-prog-time` |
  | `button.ml-step[data-step=1..16]` | each holds `<b>` (the part name) and `<small>` (`×n`) |

  Inside `svg.ml-head`:
  - `g.ml-hd` (shakes) holds the hair, the skull r 88 and the scar.
  - Per eye: `path.ml-brow.ml-brow-l|r`, the white `circle`, `g.ml-free > g.ml-spin[data-spd]` (spd 1 for the big eye,
    −2 for the small one; `.ml-free` also holds `<circle r=R fill="none">` so its fill-box centre is the eye's), and
    `path.ml-xx`.
  - Four mouth groups: `g.ml-mouth.ml-m-grit|ml-m-wait|ml-m-scream|ml-m-dead`, each with `.ml-cav`, `.ml-teeth` and
    `.ml-cav-line`.
  - `text.ml-chin` "REC" at y 81.

  The state classes:
  - On the root: `ml-st-0`…`ml-st-5`, `ml-off`, `ml-rec-on`.
  - On elements: `.sel` (track, part), `.off` (track), `.lit` (song), `.on` (keys), `.now`, `.end` and `.dead` (steps),
    `.ml-first` (on `.ml-prog`, during a first take).

- [ ] **Step 1: Write the failing static tests** in `tests/test_modgui_hooks.py`. Model them on
  `~/Fun/boreas/tests/test_modgui_hooks.py` and copy its `tags`, `attr` and `css_rules` helpers. The tests:
  - `test_knob_ports`: exactly `{"volume"}`.
  - `test_roles`: drag-handle, bypass, bypass-light, input-audio-port and output-audio-port are all present.
  - `test_keys_carry_their_ports`: `[onoff, stop_play, undo, redo, clear_track, clear_all]` in that order.
  - `test_root_scoped_and_no_shadow`.
  - `test_no_old_class_names`: no `ouroboros-*` class.
  - `test_no_ids`: no `id=` in the icon.
  - `test_styles_scoped_to_this_plugin_version`, with SCOPE `.ml-pedal__CNS__`.
  - `test_fonts_beat_modui_important`.
  - `test_footswitch_z_index_pinned`.
  - `test_animations_compositor_only`.
  - `test_knob_image_has_strip`: `clear.png`.
  - `test_head_baked`: the markers are present, with an `svg.ml-head` between them that holds the four `ml-m-*`
    groups and two `ml-spin`.
  - `test_monitored_outputs`: `modgui.ttl` names `progress`, `progress_loops`, `looper`, `take_secs` and `song_step`
    under `modgui:monitoredOutputs`.
  - `test_static_defaults` (Review Focus 5):
    - The root has `ml-st-0`.
    - Track 1 and part A have `sel`. Step 1 reads `A`/`×1`, step 2 `↺` with `end`, and steps 3–16 have `dead`.
    - `.ml-knob` has `--v:0.9091`, and `.ml-db` reads `0.0 dB`.

- [ ] **Step 2: Run `python3 tests/test_modgui_hooks.py`.** Expected: failures (the old face).

- [ ] **Step 3: Implement `head()`.** Port the mockup's `spiral`, `hairPath` and `mouth(o, 'gap', 6)` and `DEAD_MOUTH`
  exactly, with the Mohawk values (eyes at (−36, −24) R 25 and (38, −28) R 15). Write:
  - mouths at o = 0, 0.36 and 0.91, plus the dead line;
  - teeth drawn before the cavity outline, with no clip;
  - if any tooth pokes outside the cavity's corners in a render, trim that tooth's x-range in the generator, not with a
    clip.

- [ ] **Step 4: Write the template and the stylesheet.**
  - Port the mockup's `.face`, `.th-fluoro` and head rules to the class names above, using the Fluoro values. Scope
    every selector, mark every `font-family` `!important`, and name the `@font-face` families `MentalLooperAnton`
    (Anton) and `MentalLooperArchivo` (Archivo, `font-stretch 62.5% 125%`, `font-weight 100 900`).
  - Head states, from the spec's table:
    - Brows:
      - `ml-st-2` (playing): L `translateY(-4px) rotate(-14deg)`, R `rotate(10deg)`.
      - `ml-st-3` and `ml-st-4` (writing): L `translateY(4px) rotate(22deg)`, R `translateY(4px) rotate(-22deg)`.
      - `ml-st-5` (waiting): both `translateY(-8px)`.
      - All with `transition .25s`.
    - Shake: the mockup's `shake` keyframes at .16 s on `.ml-hd` for `ml-st-3` and `ml-st-4`.
    - Mouths: grit for 0–2, scream for 3–4, wait for 5, dead with `ml-off`. Animate them with `transform-box: fill-box;
      transform-origin: 50% 0`.
    - `ml-st-4` animates `.ml-free` (rotate, linear, infinite).
    - `ml-off` wins over every state: head opacity .5, `.ml-xx` shown, `.ml-free` hidden, the dead mouth, and no
      animations.
    - `ml-rec-on` turns `.ml-chin` pink.
    - `@media (prefers-reduced-motion: reduce)` stops the shake, the wobble and the tremble.
  - The badge styles key on mod-ui's `.mod-light.on` / `.mod-light.off`.
  - The footswitch gets `z-index: 26 !important`, and the light `21; pointer-events: none`, as in Boreas.
  - The knob's `.mod-knob-image` is 56 × 56 with the clear strip and `background-size: auto 56px`.
  - The template defaults are the Review Focus 5 pose.
  - Keep the audio port loops and the `mod-drag-handle` from today's template.

- [ ] **Step 5: Update `modgui.ttl`.**
  - Ports: `track`, `volume`, `rec`, `stop_play`, `undo`, `redo`, `onoff`, `clear_track`, `clear_all`, `part`, `song`.
  - Rewrite the comment: `stop_clear` is followed (it lights STOP/PLAY) but the face has no control for it, and the step
    ports are written through `set_port_value` without being listed.
  - Add `modgui:monitoredOutputs [ lv2:symbol "progress" ] , [ lv2:symbol "progress_loops" ] , [ lv2:symbol "looper" ] , [ lv2:symbol "take_secs" ] , [ lv2:symbol "song_step" ] ;`
  - Keep `modgui:brand` and `modgui:label` (the Makefile's sed keys on them).

- [ ] **Step 6: Rework `render_screenshot.py`.**
  - `page_html(pose_js: str = "") -> str`:
    - Mustache substitutions as today.
    - `/resources/` becomes `file://<modgui dir>/`.
    - mod-ui's `* { box-sizing: border-box; font-family: "cooper hewitt", sans-serif !important; }` is mirrored (see
      Boreas).
    - The script is inlined as `window.mlFace = (<script-ouroboros.js>);`.
    - `pose_js` runs after load.
  - `launch(p)`: Chromium via `executable_path=shutil.which(os.environ.get("CHROME", "google-chrome"))`.
  - 520 × 420 at scale 2, `omit_background=True`, waiting for `document.fonts.ready`.

- [ ] **Step 7: Run `python3 tests/test_modgui_hooks.py`** (all `ok`). Then render with
  `uv run --quiet --with playwright python3 plugins/Ouroboros/modgui/render_screenshot.py`, and put it beside a 1×
  capture of the mockup's box (Badge, plugin on). Positions should be within 2 px, and colours and fonts should match.

- [ ] **Step 8: Run `make test`, `make test-lv2` and `make modgui`. Commit**:
  `modgui: the Mental Looper face (template, baked head, stylesheet, monitored outputs)`.

### Task 5: The face script, its browser test, the screenshot and thumbnail

**Files:**
- Rewrite: `plugins/Ouroboros/modgui/script-ouroboros.js`.
- Create: `tests/test_modgui_face.py` (Playwright; it imports `page_html` and `launch` from `render_screenshot.py`).
- Modify: `render_screenshot.py`. Add the pose, and write `thumbnail-ouroboros.png` too.
- Modify: `Makefile`. `test-modgui` also runs `uv run --quiet --with playwright python3 tests/test_modgui_face.py`, with
  `CHROME` exported. Add a `screenshot` target.
- Regenerate: `screenshot-ouroboros.png` (1040 × 840, RGBA) and `thumbnail-ouroboros.png` (260 px wide, RGBA).

**Interfaces:**
- Consumes: Task 4's DOM, and Task 2's output symbols.
- Produces: `function (event, funcs)`, taking mod-ui's `start` and `change` events. The test harness calls
  `window.mlFace({type: 'start', icon: [host], ports: [{symbol, value}, …]}, funcs)`, and then
  `{type: 'change', icon: [host], symbol, value}`. `funcs = {set_port_value(sym, v)}` records `[sym, v, performance.now()]`
  in `window.writes`.
  - On `start`, the script reads every input from `event.ports`, including `:bypass` (1 = bypassed).
  - On `change`, it handles any input or output symbol.

- [ ] **Step 1: Write the failing browser tests** in `tests/test_modgui_face.py`. Each one loads `page_html()`, starts
  the script with the port defaults, and asserts on classes, styles and `window.writes`. Drive pointer and wheel input
  through Playwright's mouse.
  - `test_looper_states`: change `looper` 0…5; the root has exactly the matching `ml-st-N`.
  - `test_bypass`: `:bypass` 1 adds `ml-off`, 0 removes it. A start with `:bypass` 1 begins with `ml-off`.
  - `test_bypass_wins_over_state` (Review Focus 1): `looper` 3 then `:bypass` 1. `.ml-m-dead` is visible,
    `.ml-m-scream` hidden, and `getAnimations()` on `.ml-hd` and the mouths is empty.
  - `test_mouth_is_a_fixed_press`:
    - `mouse.down()` on `.ml-rec`, wait 600 ms, `mouse.up()`.
    - The writes are exactly `rec 1`, then `rec 0` 80–200 ms later. The release adds nothing.
    - The root has `ml-rec-on` until the 0.
  - `test_keys_momentary_min_hold`: a click on UNDO (down and up within 10 ms) writes `undo 1`, then `undo 0` at least
    90 ms later. A 300 ms hold writes the 0 at the release.
  - `test_key_released_on_leave` (Review Focus 3): down on CLEAR, move the mouse off the pedal, and `clear_track 0` is
    written; the key loses `.on`.
  - `test_track_click_writes_track`:
    - A click on track 3 writes `track 3`.
    - A change of `track` to 2 moves `.sel` to track 2, and `.ml-tn` reads `2`.
    - `on_2` = 0 marks track 2 `.off`.
  - `test_part_and_song`: a click on part C writes `part 2`. With `song` 0, a click on SONG writes `song 1`, and a change
    of `song` to 1 marks it `.lit`.
  - `test_step_click_cycles_part`: with `step5_part` = 2, a click writes `step5_part 3`. With `step5_part` = 5, a click
    writes 0.
  - `test_step_wheel_one_per_notch`:
    - With `step1_repeats` = 4: three `wheel` events with `deltaY -33` within 50 ms write `step1_repeats 5` once.
    - After a 300 ms pause, one `deltaY -120` writes 6.
    - The script echoes its own writes into its state, as the old script does.
  - `test_step_repeats_clamp` (Review Focus 4): at 16, a wheel up writes nothing; at 1, a wheel down writes nothing. A
    step whose part is 4 (Loop) writes nothing on wheel or drag.
  - `test_step_drag`: down on step 1 (repeats 4), move up 17 px, up. `step1_repeats 6` is the last write, and no
    `step1_part` is written.
  - `test_steps_follow_ports`: `step3_part` 4 marks steps 4–16 `.dead` and step 3 `.end` with `↺`. `song_step` 2 marks
    step 2 `.now`, and 0 clears it.
  - `test_progress_bar`:
    - `progress` .5 and `progress_loops` 4: `--p` is 0.5, with three ticks at 25 %, 50 % and 75 %.
    - `looper` 4 and `take_secs` 67: `.ml-prog` has `.ml-first`, and `.ml-prog-time` reads `1:07`.
  - `test_ticks_follow_track_switch` (Review Focus 2): `progress_loops` 4 then 1 leaves no ticks.
  - `test_eyes_follow_loop`:
    - `looper` 2, `progress_loops` 1, `progress` .25: the big `.ml-spin` has `rotate(90)`, and the small one
      `rotate(-180)` (any equivalent angle mod 360).
    - Then `looper` 1 and `progress` 0: both angles are unchanged.
    - With `progress_loops` 4 and `progress` .3: the big eye is at `frac(1.2) × 360 = 72`.
  - `test_chin_and_stop_light_follow_ports`: `rec` 1 adds `ml-rec-on`, and `stop_clear` 1 lights STOP/PLAY `.on`. `0`
    undoes both.
  - `test_volume_text_and_knob`: `volume` −6 gives `.ml-db` "−6.0 dB" (U+2212) and `--v` ≈ 0.8182. `volume` 3 gives
    "+3.0 dB".

- [ ] **Step 2: Run `make test-modgui`.** Expected: the browser tests fail, because the old script binds `.ouroboros-key`.

- [ ] **Step 3: Implement the script.**
  - Writes:
    - Keys keep today's press, release and 90 ms rule, on pointer events plus `pointerleave`.
    - The mouth sends a fixed press.
    - Steps: a click cycles the part (`(p + 1) % 6`). A drag gives one repeat per 8 px up (clamped 1–16, and a move
      suppresses the click). The wheel is the mockup's `wheelSteps` (sum `deltaY` with lines × 40 and pages × 800; a step
      at ±50; at most one per 80 ms; reset after 250 ms quiet), and scrolling up adds.
  - Follows:
    - The eyes: `rotate(frac(progress × loops) × 360 × spd)` while `looper` is 2, 3 or 5.
    - Ticks are rebuilt when `progress_loops` changes, only for 2–64 loops.
    - The time text is `m:ss` with `(s / 60) % 10`, as on the display.
    - dB is formatted as in the mockup's `render()`.
  - `--v` = `(dB + 60) / 66`.

- [ ] **Step 4: Run `make test-modgui`.** Expected: every test `ok`.

- [ ] **Step 5: Add the screenshot pose** to `render_screenshot.py`, driven through the script:
  - The ports: track 1, volume 0, song 1, part 0, and the mockup's song (A×4, B×2, A×4, B×4, C×2, Stop, then Loop).
  - The outputs: `looper` 3, `progress` .4, `progress_loops` 1, `song_step` 1.
  - Pause the animations at their start.
  - Write the screenshot, and the thumbnail at 260 px wide (Pillow, `uv run --with pillow`, or a second capture at
    `device_scale_factor` 0.5).
  - Add `test_screenshot_has_alpha` to `tests/test_modgui_hooks.py` (Boreas's version: PNG colour type 6, RGBA).
  - Run `make screenshot`. Then the new test passes, and both PNGs look like the mockup's box, screaming.

- [ ] **Step 6: Run `make test`, `make test-lv2` and `make test-modgui`. Commit**:
  `modgui: the face script, its browser test, screenshot and thumbnail`.

### Task 6: Manual, and the check on MOD Desktop

**Files:**
- Modify: `docs/manual/ouroboros-manual.html`:
  - The header image stays the screenshot (now the new face).
  - Rewrite the "The web pedal has the same actions as buttons…" paragraph (~line 310): the mouth is REC (a click
    records; holding it doesn't undo, use UNDO); tracks 1–4; parts A–D and SONG; the song steps (click: part; scroll or
    drag: repeats).
  - Add one sentence on what the head shows: the eyes turn once per loop, the mouth screams while recording, trembles
    while a REC waits, and the bar shows the selected track.
- Regenerate: `docs/manual/ouroboros-manual.pdf` (`make manual`).

- [ ] **Step 1: Edit the manual, run `make manual`,** and check the PDF's first page shows the new face, with no layout
  overflow in the header.
- [ ] **Step 2: Install both builds** (`make install` and `make install-beta`) and check in MOD Desktop, with both on
  one pedalboard. Report each item as pass or fail:
  - Each face matches the mockup, and the beta's fine print says BETA.
  - The face's outputs arrive. If not, remove `kParameterIsHidden` from the outputs, then rerun the tests and this check.
  - The eyes turn once per loop. The bar fills, and ticks on a long track, when switching tracks.
  - The mouth screams on REC and DUB, trembles with Auto Rec or Sync waiting, and closes after.
  - Holding the mouth for a second records and does not undo. Holding footswitch B still undoes.
  - The playing step follows the song, and editing a step changes the song (compare with the HMI's song page).
  - Footswitch and MIDI presses light the keys and the chin. Bypass turns the badge to OFF and the head X-eyed.
  - The two copies don't restyle each other. Four instances stay responsive.
- [ ] **Step 3: Commit**: `docs: manual: the Mental Looper face`.
