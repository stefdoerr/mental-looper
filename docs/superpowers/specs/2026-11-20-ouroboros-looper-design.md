# Design: Ouroboros — a free-time looper for the MOD Dwarf

**Date:** 2026-11-20
**Status:** draft, awaiting approval
**Working name:** Ouroboros, a placeholder chosen by Claude. Rename it through the four Makefile
variables of the template.
**Base:** `~/Fun/mod-plugin-template` (DPF, LV2/VST3/CLAP, Dwarf cross-build, BETA variant,
releases). Boreas (`~/Fun/boreas`) is the closest sibling to copy patterns from.

---

## Why

The user loops live on a MOD Dwarf with the **Looperlative LP3 Basic** plugin
(`http://looperlative.com/plugins/lp3-mod-basic`) and it falls short. The user plays **free time**:
no click, no bars, timing by hand. They want a **pure looper**: no drums, no effects, no MIDI clock.
What matters to them:

1. **Smooth overlap at the loop point, with no clicks anywhere.** LP3 loops are reported "not
   seamless" on the MOD forum.
2. **At least two tracks, locked to the length of the first recorded loop.**
3. **Overdubbing, with every overdub undoable**, one press at a time. This works like Boreas' Clear,
   which removes the newest layer.
4. **Performance.** It runs on a Dwarf next to the rest of a pedalboard.
5. **Per-track volume and a quick on/off.**

LP3 problems this design answers, from the MOD forum: no per-track start/stop, no loop position on
the display, a non-seamless loop point, and no one-press way to abort a recording. Development of LP3
has also stalled.

## Goals

- 4 mono tracks, all with the same loop length L.
- An RC-30-style record footswitch: REC → PLAY → DUB → PLAY → DUB…, with a setting for
  REC → DUB. Its label always shows what the next press will do.
- An undo footswitch: tap = undo, hold = redo. Undo history covers every layer and is bounded only by
  a memory budget.
- Click-free everywhere: the loop point, punch in and out, undo and redo, on/off, volume, clear.
- Knobs act on a **selected track**: knob 1 selects the track, knob 2 is on/off, knob 3 is volume.
- A **progress bar** and track states on the Dwarf display, using Starless 19 display widgets.
- Memory follows what is actually recorded. An empty track costs no RAM.

## Non-goals (v1)

- Drums, rhythms, click track.
- Effects of any kind: reverse, half speed, pitch, feedback/decay, filters.
- MIDI clock in or out, host tempo sync, quantising to bars, fixed lengths, count-in. Bar snapping
  may come later as an option.
- Saving loops across a pedalboard change or reboot. Loops live only for the session.
- Stereo. The plugin is mono in and mono out.
- Song parts (A/B sections), track lengths that are multiples of L, auto-record on an input threshold.
- Custom modgui art. v1 uses a plain functional modgui from the template. An art pass, like Boreas'
  ice chunks, can come later.
- A full-screen display widget. Only the one widget described below is in v1.

## Anti-goals (a failure even if it "works")

- **Any audible click or gap** at the loop point or on any state change.
- **A late footswitch.** REC/PLAY/DUB must act on the press, never on release or after a hold
  timeout.
- **Loop drift.** A track must never slide against the others.
- **Audio-engine death from memory.** The Dwarf has no swap, and running out of RAM kills jackd.
- **Allocating, locking or blocking in `run()`** beyond what is stated under Constraints.

---

## Constraints (measured or read from source)

- **Dwarf footswitches.** A plugin gets only **two** footswitches per page, B and C. Footswitch A turns
  the footswitch pages (`mod-dwarf-controller/app/inc/config-moddwarf.h:153`,
  `MAX_FOOT_ASSIGNMENTS = FOOTSWITCHES_COUNT - 1`).
- **Dwarf knobs** are endless encoders with one binding per subpage. Pressing a knob flips a toggle
  bound to it (`mode_control.c` `CM_toggle_control`). A knob bound to a continuous control ignores
  presses.
- **RAM.** The Dwarf has 993 MB total. About 575 MB is available with the user's current pedalboard,
  and there is **no swap** (measured 2026-11-20).
- **No memory locking of plugin memory.** jackd, which hosts mod-host in-process, has
  `VmLck` 81 MB against `VmRSS` 297 MB, so it does not `mlockall(MCL_FUTURE)`. Memory that is reserved
  but never touched costs no RAM.
- **Footswitch timing resolution.** Footswitch events reach the plugin as control-port changes at
  block start. That is 64–256 frames (1.3–5.3 ms at 48 kHz). Starless 19 / MOD OS 1.14 add 64-frame
  buffers on the Dwarf, which makes performance at 64 frames matter.
- **MOD HMI calls take a mutex.** In mod-host, `set_label` / `set_led_with_blink` lock
  `g_hmi_mutex` and write to the HMI serial queue (`mod-host/src/effects.c:4015`). They are
  acceptable from `run()` **only on state changes**, never every block. LP3 does the same.
- **Removing a port breaks saved pedalboards** (mod-ui KeyError on boot). Never delete a port after
  the first release. Deprecate it instead (see the template's `INSTRUCTIONS.md`).
- **Display widgets** exist only on Starless 19 or later and alabs-mod-desktop. On stock MOD OS and
  MOD Desktop the plugin must work fully, just without the widget. The one exception is the volume
  knob, which is one step off after a track switch without the mod-ui patch. The MOD HMI extension
  (labels and LEDs) works on stock MOD OS 1.11 and later.

---

## Approaches considered

The footswitch labels, the LEDs and the display widget need two LV2 extensions that DPF does not pass
through: the MOD HMI extension (`http://moddevices.com/ns/hmi`, header
`mod-host/src/lv2/lv2-hmi.h`) and aLabs HMI widgets
(`https://github.com/sejerpz/alabs-hmi-widgets`). DPF's LV2 wrapper
(`dpf/distrho/src/DistrhoPluginLV2.cpp`) neither hands arbitrary host features to the plugin nor
answers arbitrary `extension_data`.

1. **Template + small DPF patch. Picked by the user.** Patch DPF's LV2 wrapper so it passes the two
   host features to the plugin and answers the two plugin interfaces. All of the template's
   infrastructure is kept. The cost is carrying a patch on the DPF submodule and re-checking it when
   DPF is updated.
2. **Plain LV2, no DPF**, as in the aLabs tutorial. Nothing to patch, but the TTL is hand-written, the
   template's build and release scripts have to be reworked, and the desktop builds are lost.
3. **DPF without the extensions.** Dropped: it loses the progress bar and the "next action" labels,
   both of which the user asked for.

---

## Behaviour

### Tracks and the loop length

- 4 tracks, mono. Every track plays in lock-step from one shared loop position, so drift is
  impossible by construction.
- **L is unset while every track is empty.** The first recording on *any* track sets L.
- Once L is set, recording on another track is an **overdub onto silence of length L**. It starts
  wherever you press, wraps around the loop end, and keeps going until you press again. That makes it
  the track's first layer.
- **L resets** when every track becomes empty, whether through undo or Clear all. The next recording
  sets a new L.
- **Max L = 5 minutes.** Picked by the user. Reaching it closes the loop automatically, as if B had
  been pressed.
- *Claude's call:* a recording shorter than about 100 ms counts as an accidental double press and is
  treated as aborted.

### Footswitch B — Rec/Play/Dub (on the selected track)

| Track state | Press B → | Label shown (next action) |
|---|---|---|
| Empty | starts recording | `REC` |
| Recording | closes the recording → Playing (or Overdubbing in REC→DUB mode) | `PLAY` (or `DUB`) |
| Playing | starts overdubbing | `DUB` |
| Overdubbing | stops overdubbing → Playing | `PLAY` |

- A setting, **After Rec**, chooses Rec → Play (default) or Rec → Dub.
- B acts **on the press**. It is bound as a trigger (`kParameterIsTrigger`), like Boreas' Freeze.
- **One-press handover (decided by the user).** Only one track records at a time.
  - Pressing B on a track while *another* track is recording or overdubbing closes the running one, as
    if its own B had been pressed, and starts recording on the selected track in the same press.
  - Before L is set: record track 1, turn knob 1 to track 2, press B. Track 1's first recording closes,
    which sets L, and track 2 starts recording from position 0, in one press.
  - During the overlap (the closing track's tail or punch-out fade), the input is split as in Rec→Dub:
    g·x to the closing track, (1−g)·x to the new one. The sum stays at unity, and undoing either layer
    leaves the other smooth.
- An **off** track can still be recorded or overdubbed. *Claude's call:* you hear yourself through
  the dry path but not the track until you switch it on.

### Footswitch C — Undo / Redo (on the selected track)

- Bound **momentary**: the TTL is patched with `mod:preferMomentaryOnByDefault`, as Boreas does for
  Hold (`boreas/Makefile:110`). The plugin times the press itself.
- **Separate `undo` and `redo` trigger ports** exist for the web UI and MIDI. The momentary
  `undo_redo` port is for the footswitch only and is left off the modgui face. Lesson from Boreas:
  its Clear went from momentary tap/hold back to a trigger because "a momentary Clear made the
  on-screen button sticky and the tap-vs-hold timing fiddly" (`BoreasPlugin.cpp:106`). The user still
  chose tap/hold on the footswitch. The hold time is tunable, and the web buttons avoid the
  stickiness.
- **Tap** (released before the hold time) = **undo** when the foot lifts.
- **Hold** past the hold time = **redo** at the moment the threshold is reached, so you feel it while
  still standing on it. The hold time defaults to ~500 ms, exposed as a setting for tuning on the
  device.
- Undo removes the newest layer of the selected track. Repeated undos peel layers down to the first
  recording, and undoing that leaves the track **empty** (B shows `REC` again).
- *Claude's call:* **undo while overdubbing** cancels the overdub in progress and goes to Playing.
  **Undo while making the first recording** (L not yet set) aborts it, and the track is empty. This
  answers the "can't abort a recording" complaint about LP3.
- *Claude's call:* **one layer per press.** An overdub that runs for several passes is one layer, and
  one undo removes all of it. Mobius instead makes a layer per pass.
- *Claude's call:* **silent overdubs are dropped.** The plugin tracks the peak of what it wrote during
  the overdub. If it never rises above a fixed floor (≈ −66 dBFS; Mobius uses ≈ −68), no layer is
  made. This catches truly silent input, e.g. a guitar volume knob at zero or a noise gate before the
  looper. It cannot know you "didn't play": if hum or hiss came in above the floor, the overdub did
  add that to the loop, so it rightly becomes a layer that undo removes.
- A new recording or overdub on a track discards that track's redo history.
- When L is reset, all redo history is discarded too, since the old layers have the wrong length.

### Knobs (all act on the selected track)

| Knob | Control | Notes |
|---|---|---|
| 1 | **Track** 1–4 (enumeration) | Changes which track B, C, knob 2 and knob 3 act on. Changing it does nothing to the audio. |
| 2 | **On/Off** (trigger: each knob press flips the selected track) | Mutes or unmutes the selected track. The track keeps running in time while off. The screen shows its real state (`ON`/`OFF`) via `set_value`. |
| 3 | **Volume** (proxy for the selected track) | Volume of the selected track. *Claude's call:* in dB, −60…+6, default 0. |

**Why On/Off is a trigger and Volume needs a patch** (found by the builder check, verified in source).
When a plugin changes its own control (a control-input-port change request), mod-host passes it to
mod-ui (`mod-host/src/effects.c:4350` → `mod-ui/mod/host.py:1859`). mod-ui stores it and updates the
browser, but **never tells the Dwarf controller**. The controller keeps its own copy of every knob
value and steps from that. So:
- **On/Off** is a trigger. A press means "flip the selected track", whatever that track's state, so it
  needs no stored value and works on stock MOD OS. *Claude's call.*
- **Volume** is a proxy. When the selected track changes, the plugin moves it to that track's value
  with a change request (DPF `DISTRHO_PLUGIN_WANT_PARAMETER_VALUE_CHANGE_REQUEST`, already enabled in
  Boreas). **The mod-ui patch below** forwards that change to the controller, so the next turn steps
  from the right value. The encoders are endless, so nothing jumps.
  - **On stock MOD OS** without the patch, the first turn after a track switch starts from the old
    track's value, one step off. Accepted by the user.

*Claude's call:* **volume is a playback fader only.** It never changes the stored audio, so undo stays
exact. Overdubbing onto a track turned down to −6 dB stores the new playing at full level, so it also
plays back 6 dB down. Turning a track down is also the way to tame build-up without touching layers.

**Per-track ports.** `on_1..on_4` and `volume_1..volume_4` are real ports, saved with the pedalboard
and editable from the web UI. They are the source of truth. The knob-bound Volume proxy follows the
selected track. The builder must keep proxy and per-track ports in sync without echo loops.

*Claude's call:* knob labels follow the track via `set_label`, as `On 2` / `Vol 2`, so it is always
clear which track a knob acts on.

### Other controls (web UI / bindable, not on the default layout)

- **After Rec**: Rec→Play / Rec→Dub.
- **Clear track**, **Clear all**: triggers. Click-free.
- **Tail** (ms), **Hold time** (ms), **Memory budget** (MB), **Offset** (ms), **Trigger latency**
  (ms): tuning settings, see below.
- **Dry thru**: toggle, default off (see Dry signal).
- **Bypass** (the template's click-free bypass). *Claude's call:* bypassed = the loops are muted but
  keep running in time, so un-bypassing resumes in place. The output is the dry signal if Dry thru is
  on, otherwise silence. Passing input through on bypass would double the dry signal on a parallel
  board.

### Timing and latency

- **No overdub latency compensation by default.** The Dwarf has no analog dry path: the player hears
  their dry signal through the same converters as the loops and times their playing to that. So input
  arrives in the plugin exactly when the matching loop sample is output, and writing at the playhead is
  correct. SooperLooper switches its compensation off when monitoring through it, and the Mobius manual
  says to disable compensation when you monitor through the computer (§9.9).
  - An **Offset** setting (signed, ms, default 0) shifts where overdubs are written, for players who
    time differently (Mobius manual §10).
- **Footswitch presses arrive late.** The path is Dwarf controller → serial → mod-ui (Python,
  `hmi_parameter_set`) → mod-host. That adds latency and jitter beyond block granularity, unmeasured.
  A constant delay doesn't change L, but it cuts off the attack of the first note.
  - The plugin keeps about **100 ms of input history** and back-dates the start of a recording and of
    each punch-in by a **Trigger latency** setting (default about one block, tuned on the device).
    SooperLooper does the same with a 32768-sample input ring and trigger latency = one buffer.
  - The close of the first recording is back-dated by the same amount, so L is unaffected.

### Dry signal

**Decided by the user:** by default the output is **only the loops**: the sum of all on tracks × their
volumes. The dry signal is routed around the looper on the pedalboard (a parallel path).
- A **Dry thru** option (toggle, default off) adds the dry input at unity, with zero latency, for
  boards that run the looper in series.
- With the default, the user's current boards (input → LP3 → gain, in series) need re-wiring with a
  parallel dry path.

---

## Click-free audio

### The loop point ("smooth overlap"), confirmed by the user

When the first recording is closed at position L:

1. The loop is samples `[0, L)`.
2. **Recording continues for a tail of length T** past the press. That captures the ring-out of
   whatever was being played.
3. The tail is **added onto the start of the loop** with a fade-out envelope (1 → 0 over T). The very
   start of the recording gets a short fade-in of a few ms, which removes the "recording started
   mid-waveform" step.

At the boundary the output goes from sample `L-1` to `start[0] + tail[0]`. `start[0]` is ≈ 0 because
of the fade-in, and `tail[0]` is the sample that really followed `L-1` in time, so the waveform is
continuous. A chord still ringing at the loop point carries into the next pass instead of being cut
off. Nothing at the start is faded beyond the first few ms, so the attack of the first note survives.

**The tail length is a trade-off** (from the software-looper research). Free loopers usually close the
loop by pressing *as they re-strike the first note*. A long tail captures that re-struck note at almost
full gain and adds it onto the original first note, which gives a doubled or flammed downbeat on every
pass. Mobius refuses to do an automatic tail for this reason. Loopy Pro uses a 50 ms boundary
crossfade and warns that it can blur transients near the loop start.
- **Default T = 50 ms**, *Claude's call*. Exposed as the **Tail** setting, 0–500 ms, to be tuned by
  ear on the device.
- For a long ring-out, the clean way is **Rec→Dub mode**: keep playing past the close and stop the
  overdub when the ring-out is done.
- T is clamped to at most L/2, so the tail can never wrap onto itself on a very short loop.

**Overdubs:**
- a short fade-in at punch-in;
- a short fade-out at punch-out (3–10 ms, no tail). A tail here would record a fading ghost of
  whatever you play next;
- the overdub wraps around the loop end freely, and its samples always add onto the delta, never
  overwrite it.

**Rec→Dub and undo.** In Rec→Dub mode the audio right after the close continues into the first
overdub. If that overdub is later undone, layer 1 would end mid-waveform and click. Fix: for the first
T samples after the close, split the input as g·x into layer 1 (the tail, g: 1 → 0) and (1−g)·x into
layer 2. The sum is exact and each layer is smooth on its own. Mobius solves the same problem with a
deferred fade window (`FadeWindow.cpp`).

The short fade lengths are the builder's call. References: SooperLooper 64 samples, Mobius 128 frames
clamped to 16–256, loopor 32 samples.

### Everything else

On/off, volume, clear, undo and redo must all ramp, over ~10–20 ms, *Claude's call*.

**Undo, redo and clear must never do O(L) work in one block.** Subtracting a 5-minute delta touches
14.4 M samples, far more than fits in a 1.3 ms block. So apply them **lazily at the playhead over one
pass**:
- the output reads `buffer − pending delta` (or `+` for redo) until the playhead has passed each
  sample;
- the subtraction is committed sample by sample as the playhead goes by;
- the switch-over at the playhead ramps over the same ~10–20 ms.

SooperLooper and Mobius avoid this by keeping full layer copies, so their undo is a pointer swap. We
trade that memory for this deferred commit. Several quick undos queue several pending deltas, and the
builder bounds the queue.

### Output headroom

Overdub layers add up: two equally loud, unrelated layers are about +3 dB louder on average, and
their peaks can stack to +6 dB. With 4 tracks at up to +6 dB, plus dry when Dry thru is on, the sum
can exceed 0 dBFS. The buffers are float, so stored audio never clips. Only the output can. SooperLooper scales old audio
by 0.96 on each overdub pass to avoid build-up. We can't do that without breaking subtractive undo.
*Claude's call:* a soft clip on the output only, transparent below about −3 dBFS. It's a safety net,
not an effect.

---

## Memory

- **The track buffers are reserved, not filled.** Each track reserves room for max L (5 min × sample
  rate × 4 bytes ≈ 57.6 MB at 48 kHz, ≈ 230 MB virtual for 4 tracks) at instantiate, using
  untouched memory (mmap or calloc of a fresh mapping). Linux backs it with RAM only as samples are
  written. An empty track costs nothing physical.
  - First-touch page faults in `run()` happen only while recording new audio: about 50 per second of
    mono audio, which is negligible. Re-recording into pages already touched causes none.
- **Undo history stores deltas, not copies.** An overdub layer stores only the span it actually
  covered, from punch-in to the end of its tail, wrapping, at most L, as the samples it added. Undo
  subtracts that delta from the track buffer. Redo adds it back. Overdubs are purely additive (no
  feedback/decay), so this is exact to float rounding. The first recording of a track needs no
  delta, because undoing it simply empties the track.
- **The memory budget**, default ~200 MB and exposed as a setting, caps the **undo and redo deltas**.
  When an overdub needs more room, even mid-overdub, space is freed in this order, *Claude's call*:
  1. redo history is dropped first, oldest first, across all tracks;
  2. then the **globally oldest undo layers are merged**, whichever track they belong to;
  3. if the overdub in progress alone exceeds the budget, its delta is dropped. It keeps recording but
     can't be undone.

  Merging just drops a layer's delta from the history: the track buffer already contains it, so it
  costs nothing, but it can no longer be undone. This matches the user's chosen fallback: every layer
  stays undoable until memory runs out. SooperLooper instead stops the overdub when memory runs out,
  which we reject.
  - Scale: 200 MB is about 3.5 full-length overdubs at L = 5 min, but about 130 at L = 20 s.
- **Reused memory holds old audio.** After Clear or undo-to-empty, a track's first layer must
  **overwrite** its buffer, never add to it.
- **Worst case** = 4 × L of track buffers (≤ 230 MB at 5 min) + the budget (200 MB), about 430 MB
  against ~575 MB available. The budget is a setting so the user can lower it on a heavy pedalboard.
- **No allocation in `run()`.** Deltas come from a region reserved at instantiate. A pool of
  fixed-size blocks is suggested, because layers are freed LIFO per track but interleaved across
  tracks. The layout is the builder's call.
- **Known limit, accepted:** clearing does not hand memory back to the OS. The next recording reuses
  the touched pages. Releasing it (`madvise(MADV_DONTNEED)` from a non-RT thread) is a later add-on
  if it matters.
- All sizes come from the actual sample rate, not a hard-coded 48 kHz.

---

## Display

### Footswitch labels and LEDs (MOD HMI extension, works on stock MOD OS ≥ 1.11)

- B's label = the next action (`REC` / `PLAY` / `DUB`) for the selected track.
- B's LED shows the selected track's state. *Claude's call:* reuse LP3's scheme, which the user
  already knows:
  - solid white = empty;
  - blinking red = recording;
  - blinking green = overdubbing;
  - solid green = playing.
- C's label is `UNDO`. *Claude's call:* show `—` when there is nothing to undo.
- Call `set_label` / `set_led_with_blink` **only when something changes**: state, selected track or
  addressing (see Constraints). `addressed()` / `unaddressed()` of `LV2_HMI_PluginNotification` give
  the `LV2_HMI_Addressing` handle of each bound port.

### The "Loop" widget (aLabs HMI widgets, Starless 19+)

Placed by the user in Starless' Display dialog. The intended spot is the column above knob 1
(40 × 37 px). It replaces knob 1's default content, so it shows the track number itself, and it
leaves the footswitch labels visible.

```
 ┌──────────┐
 │ T2   DUB │   selected track + its state (the label of its next action, or its state)
 │ ■ □ · ·  │   tracks 1-4: ■ on, □ off, · empty, blinking = recording/overdubbing
 │▓▓▓▓▓░░░░░│   loop position: fills left→right over one pass
 └──────────┘
```

- **While the first recording runs** (L unknown), the bar area shows the elapsed time (`0:07`)
  instead.
- **It adapts to the area it is given.** It declares a small minimum (about one footer, ≤ 51 × 8 so it
  is offered on the Dwarf and the Duo X) and a natural size of 40 × 37. In a short area it draws the
  bar alone, or the bar plus marks. It never hard-codes a size.
- 1-bit `Mono1`. It draws from `run()` with `draw_bitmap()`, which is RT-safe and only queues. It
  redraws only what changed: the bar when its pixel position moves, the rest on state changes. It
  keeps the widget handle in an atomic, as the aLabs example does.
- Pixel-level look (font, glyphs, spacing) is the builder's call, previewed on alabs-mod-desktop.

---

## The DPF patch

Kept in the plugin repo, for example as `patches/dpf-lv2-hmi.patch`, and applied at build time so the
submodule stays pristine. The mechanism is the builder's call. It must:

1. **Pass host features to the plugin.** In `lv2_instantiate`, capture `LV2_HMI__WidgetControl`
   (`LV2_HMI_WidgetControl*`) and `ahw:WidgetDraw` (`ALABS_HMI_WidgetDraw*`), and make them reachable
   from the `Plugin` subclass. Both are optional: null when the host lacks them.
2. **Answer the plugin interfaces.** In `lv2_extension_data`, return `LV2_HMI__PluginNotification`
   (`addressed` / `unaddressed`) and `ahw:WidgetNotification` (`widget_shown` / `widget_hidden`),
   forwarding to the plugin instance.
3. **TTL.** Declare `lv2:optionalFeature` for both features, `lv2:extensionData` for both
   interfaces, the `ahw:widget` block, and `mod:preferMomentaryOnByDefault` on C. Use the template's
   Makefile TTL-patch step, the same `sed` approach Boreas uses.

Headers are vendored: `lv2-hmi.h` (from mod-host) and `alabs-hmi-widgets.h`.

Threads: `widget_shown` / `widget_hidden` and `addressed` / `unaddressed` come from non-RT threads,
possibly while `run()` runs. Share state with `run()` through atomics only.

---

## The mod-ui patch (decided by the user)

A separate, small change in mod-ui. It is not part of the plugin.

- **What:** when mod-host reports a `param_set` that a *plugin* requested, and that port is bound to a
  Dwarf control, also send the new value to the controller. In `mod-ui/mod/host.py`, the
  `param_set` branch of the mod-host message handler (around line 1859) stores the value and notifies
  the browser. Add: look up the port's HMI addressing and push the value with the existing
  `addr_task_set_value` → `hmi.control_set` path (around line 1039).
  - Mind enumerations, which that path reloads instead of setting.
  - Don't echo values the controller itself just sent.
- **Why it's general:** any plugin that changes its own knob-bound controls benefits. That makes it a
  natural candidate for upstream Starless (`sejerpz/mod-ui`), where the user has contributed before.
  Sending it upstream is the user's decision.
- **Where:** the user's mod-ui checkout (`~/Fun/mod/mod-ui`). Deploy it to the Dwarf the same way as
  earlier mod-ui work. Rebase it onto the stls19 tag the Dwarf runs.
- **Test:** on the device, bind Volume to knob 3, switch tracks with knob 1, turn knob 3 one step. The
  value must step from the new track's volume.

---

## Performance

- The per-sample work is: read 4 track buffers, sum with smoothed gains, and write the input into one
  track while recording or overdubbing (two during a handover overlap). There are no FFTs and no filters. CPU should be well
  below LP3. *Claude's call:* measure with the mod-ui CPU meter at 64 and 128 frames on the Dwarf,
  using a BETA build side by side with LP3.
- Flush denormals at the top of `run()`, as `BoreasPlugin.cpp` does.

## Testing

Unit tests in `tests/`, in the style of Boreas' `test_*.cpp`:
- **State machine:** every B/C transition, both After-Rec modes, undo/abort while recording and
  overdubbing, L reset when everything is empty, max-L auto-close, one-press handover (including
  before L is set), tap vs hold timing on C.
- **Seam continuity:** record a sine and close at an arbitrary sample. There must be no sample-to-sample
  step at the loop boundary larger than the sine's own maximum step. Same check at punch in and out.
- **Undo/redo exactness:** after undo the buffer equals the pre-overdub buffer within ~1e-6, and after
  redo it equals the post-overdub buffer.
- **Budget merge:** filling the budget drops redo first, then merges the globally oldest layers. Undo
  still works for the rest. An oversized overdub keeps recording without becoming a layer.
- **Rec→Dub seam:** in Rec→Dub mode, undoing the first overdub leaves no step at L−1 → 0.
- **Lazy undo:** no single block does more than a bounded amount of undo work. The output equals the
  committed result throughout.
- **Empty overdub:** a silent DUB → PLAY creates no layer.
- **Tail clamp:** a loop shorter than 2T works and has no self-overlap.
- **Ramps:** on/off, volume, clear and undo produce no step larger than a threshold.
- **Lock-step:** after many passes and mixed actions, every track is still aligned to sample 0 of L.

On the device: a BETA build next to LP3, listening for clicks and checking CPU and xruns at 64 frames.

## Left to the builder

- The block-pool layout and the delta bookkeeping.
- Default values for the tail, the short fades, the ramps and the hold time, tuned on the device.
- Port symbols and names (keep them stable from the first release), port groups, the plain modgui.
- The widget's pixel design.
- How the DPF patch exposes features to the `Plugin` subclass.
- Keeping the proxy and per-track ports in sync.
