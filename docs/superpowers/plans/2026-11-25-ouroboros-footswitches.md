# Footswitches B and C Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Holding B undoes a layer (never the first); C turns the selected track off/on, clears it on a hold, stops or
starts everything on a double-tap, and starts the selected track alone from a stop; Redo moves to knob 2.

**Architecture:** Gesture timing stays in the plugin (`OuroborosPlugin.hpp` `run()`), where C's tap/hold/double-tap
live today; B gets the same press/hold handling. The engine (`dsp/Looper.hpp`) gains three control calls (`peel`,
`cancelWait`, `closeTake`); its audio path is untouched. Labels move in `hmi/HmiBridge.hpp`.

**Tech Stack:** C++14, DPF (LV2), MOD Dwarf HMI (lv2-hmi, alabs widgets), the repo's own test harness (`make test`,
`make test-lv2`).

**Spec:** `docs/superpowers/specs/2026-11-25-ouroboros-footswitches-design.md` (approved). Read it before any task.

## Global Constraints

- The engine's audio path is untouched: no existing `Looper` function is edited; the only engine change is the three new
  public methods of Task 1. Gestures are block-rate control logic in `run()`.
- Gestures are timed in whole audio blocks, as C's are today (`fHold += frames`, at most one block of error).
- Hold Time 200–2000 ms, default 500 ms (188 blocks of 128 at 48 kHz); Double-Tap 150–600 ms, default 400 ms (150
  blocks). Values unchanged.
- Ports: `rec` (name `Rec/Undo`) becomes momentary; `undo_redo` becomes `stop_clear` (name `Stop/Clear`), momentary; the
  parameter index order is unchanged (`kParamStopClear` takes `kParamUndoRedo`'s index 1). Momentary =
  `mod:preferMomentaryOnByDefault`, added by the top-level `Makefile`'s `ttl` target (its `for sym in ...` list).
- No DPF change (the local `patches/dpf-lv2-hooks.patch` is not touched).
- Labels, exact: C shows `ON n` / `OFF n` while running, `PLAY n` while stopped, `-` for an empty selected track (n =
  selected track 1–4); knob 2 shows `REDO` or `-`.
- Not in production: no compatibility shims for old symbols or saved pedalboards.
- `make test` and `make test-lv2` pass at the end of every task. Commits end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Rulings made while planning

- **C's tap while stopped turns off only the other tracks that hold audio;** empty tracks keep their flag. The spec says
  "every other track"; turning an empty track off would leave its next recording silent, the very reason the spec gives
  for "a tap on an empty track does nothing". Cost if wrong: one condition.
- **"The part that will start" is the engine's current part (`part()`).** A stop with Song on parks in step 1's part
  already (`park()` switches to the queued part), so no new engine query is needed.
- **A B press while C's first-recording tap is still pending applies the abort first,** then B's press acts (today's
  rule for a pending undo, `bAfterATapUndoesFirst`).
- **C has no LED handling** (the spec leaves LED colours to the builder; none today).

## Review Focus

1. A hold on B whose press closed a first recording with After Rec = Rec > Dub: the recording stays, only the overdub
   the press started is dropped. (Task 2, `holdBAfterRecDubKeepsTheRecording`)
2. Track turned during a C hold: the track pressed is the one cleared, not the newly selected one. (Task 3,
   `cGestureKeepsTheTrackItWasPressedOn`)
3. A double-tap whose first tap turned the track off with Stop Mode = Fade: the track is back on for the fade-out.
   (Task 3, `cDoubleTapRestoresBeforeAFadeStop`)
4. A part switch landing during a C hold: the new part's track is not cleared. (Task 3, `cGestureDroppedOnPartChange`)
5. C's solo start changes On flags, so a later Load asks `LOAD n?`. (Task 4, `cSoloStartCountsAsAChange`)

---

### Task 1: Engine calls for the new gestures

**Files:**
- Modify: `plugins/Ouroboros/dsp/Looper.hpp` (public section, next to `undo()` / `redo()`)
- Create: `tests/test_footswitch.cpp` (engine tests on `tests/looper_rig.hpp`'s `Rig`; `make test` picks up
  `tests/test_*.cpp` by wildcard)

**Interfaces:**
- Produces:
  - `void Looper::peel(int t)`: B's hold. A press wait of t's own (`pend_.kind == Pending::Press && pend_.t == t`) is
    cancelled first; an overdub running on t is dropped (as `undo()` cancels one: `endDub(tr, true)`); then, if t has a
    layer above its first (`undoTop > 0`), the newest is undone exactly as `undo()` undoes a layer (respecting
    `kMaxPendingOps`). Never aborts a recording, never undoes the first layer (the base), never touches an arm or
    another track's wait.
  - `bool Looper::cancelWait(int t)`: cancels t's Auto Rec arm, t's press wait, or a pending restart (which belongs to
    every track); true if one was cancelled. Another track's arm or wait is left alone.
  - `void Looper::closeTake(int t)`: t recording onto the loop or overdubbing closes as its B press would, ending in
    Playing whatever After Rec says (`close(t, false)`). The first recording (`recTrack_ == t`) and any other state:
    nothing.

- [ ] **Step 1: Write the failing tests** in `tests/test_footswitch.cpp` (style of `tests/test_tracks.cpp`: `CHECK`,
  a `main()` calling each, `Rig r;`, `recordBase(r, n, amp)` from the rig):

```cpp
static void dub(Rig& r, uint32_t n) { r.lp.pressRec(0); r.feed(sine(330, 0.1f, n)); r.lp.pressRec(0); r.silence(4800); }

static void peelUndoesTheNewestLayerButNeverTheFirst() {
    Rig r; recordBase(r, 24000, 0.2f); dub(r, 6000); dub(r, 6000);
    CHECK(r.lp.undoDepth(0) == 2);
    r.lp.peel(0); r.silence(1000); CHECK(r.lp.undoDepth(0) == 1);
    r.lp.peel(0); r.silence(1000); CHECK(r.lp.undoDepth(0) == 0 && r.lp.state(0) == ouro::TrackState::Playing);
    r.lp.peel(0); r.silence(1000); CHECK(r.lp.undoDepth(0) == 0 && r.lp.state(0) == ouro::TrackState::Playing);
}
static void peelDropsARunningOverdubThenUndoes() {
    Rig r; recordBase(r, 24000, 0.2f); dub(r, 6000);                    // depth 1
    r.lp.pressRec(0); r.feed(sine(330, 0.1f, 6000));                     // a second overdub running
    r.lp.peel(0); r.silence(1000);
    CHECK(r.lp.state(0) == ouro::TrackState::Playing && r.lp.undoDepth(0) == 0);
}
static void peelLeavesRecordingsAndArmsAlone() {
    Rig r; r.lp.pressRec(0); r.feed(sine(220, 0.1f, 4800));
    r.lp.peel(0); CHECK(r.lp.state(0) == ouro::TrackState::Recording);   // the first recording runs on
    Rig a; a.lp.setAutoRec(true); a.lp.pressRec(0); a.lp.peel(0); CHECK(a.lp.armedTrack() == 0);
}
static void cancelWaitIsPerTrack() {
    Rig r; recordBase(r, 24000, 0.2f); r.lp.setAutoRec(true); r.lp.pressRec(2);   // track 3 armed
    CHECK(!r.lp.cancelWait(0) && r.lp.armedTrack() == 2);
    CHECK(r.lp.cancelWait(2) && r.lp.armedTrack() == -1);
}
static void closeTakeEndsInPlaying() {
    Rig r; recordBase(r, 24000, 0.2f); dub(r, 6000);
    r.lp.pressRec(0); r.feed(sine(330, 0.1f, 6000));
    r.lp.closeTake(0); CHECK(r.lp.state(0) == ouro::TrackState::Playing && r.lp.undoDepth(0) == 2);
    r.lp.setRecToDub(true); r.lp.pressRec(1); r.feed(sine(440, 0.1f, 30000));   // track 2 records onto the loop
    r.lp.closeTake(1); CHECK(r.lp.state(1) == ouro::TrackState::Playing);      // not Overdubbing
    Rig f; f.lp.pressRec(0); f.feed(sine(220, 0.1f, 4800));
    f.lp.closeTake(0); CHECK(f.lp.state(0) == ouro::TrackState::Recording);    // the first recording: nothing
}
```

  Also `peelCancelsItsTracksPressWait`: with a grid that makes a press wait (set up as `tests/test_sync.cpp` does), a
  press on track 0 waits (`userWaiting(0)`); `peel(0)` → `!r.lp.userWaiting()`.

- [ ] **Step 2: Run `make test`.** Expected: `tests/test_footswitch.cpp` fails to compile (`peel` not a member).
- [ ] **Step 3: Implement** the three methods in `Looper.hpp` (public, after `redo()`), each with a one-line comment in
  the file's style. Reuse `cancelWaiting()`, `endDub()`, `addOp()`, `close()`; do not edit them.
- [ ] **Step 4: Run `make test`.** Expected: `==> all tests passed`.
- [ ] **Step 5: Commit** `tests/test_footswitch.cpp` and `Looper.hpp`: `engine: peel, cancelWait and closeTake for the
  footswitches`.

### Task 2: Ports, and holding B undoes

**Files:**
- Modify: `plugins/Ouroboros/OuroborosPlugin.hpp` (ParamIndex enum; the `defs` table; `hmiPort()`; the trigger block
  in `run()` around `rising(kParamRec)`; members near `fHold`)
- Modify: `Makefile` (`ttl` target momentary list)
- Modify: `tests/test_plugin.cpp` (symbol list in `layout()`; `kParamUndoRedo` → `kParamStopClear` everywhere; new
  tests), `tests/lv2/test_lv2_hooks.cpp` (momentary check)
- Modify: `plugins/Ouroboros/modgui/icon-ouroboros.html` comment (`undo_redo` → `stop_clear`)

**Interfaces:**
- Consumes: `Looper::peel(int)` (Task 1).
- Produces: `kParamStopClear` (index 1, symbol `stop_clear`, name `Stop/Clear`, kind `Bool`); `kParamRec` symbol `rec`,
  name `Rec/Undo`, kind `Bool` (no longer a trigger); in `tests/test_plugin.cpp` the helpers
  `bDown()`, `bUp()`, `cDown()`, `cUp()` (set the port to 1 or 0, then `runN(1)`) and `tapC()` (`cDown(); runN(19);
  cUp();`, about 53 ms) that Tasks 3 and 4 use.

- [ ] **Step 1: Write the failing tests** in `tests/test_plugin.cpp` (B taps stay `set(kParamRec, 1); runN(1);
  set(kParamRec, 0);` as in `recordBaseAndDub()`; a hold is `bDown(); runN(188);`):

```cpp
static void dubB(int blocks) { set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(blocks); set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(40); }

static void holdBUndoesOneLayerPerHold() {
    fresh(); recordBaseAndDub(); dubB(40);                              // depth 2
    bDown(); runN(188); CHECK(eng().undoDepth(0) == 1 && eng().state(0) == ouro::TrackState::Playing);
    bUp(); runN(10);
    bDown(); runN(188); CHECK(eng().undoDepth(0) == 0); bUp(); runN(10);
    bDown(); runN(188); CHECK(eng().undoDepth(0) == 0 && eng().state(0) == ouro::TrackState::Playing);  // first kept
    bUp();
}
static void holdBDuringAnOverdubUndoesIt() {
    fresh(); recordBaseAndDub();                                        // depth 1
    set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(60);           // an overdub running
    bDown(); CHECK(eng().undoDepth(0) == 2);                            // the press closed it
    runN(188); CHECK(eng().undoDepth(0) == 1); bUp();
}
static void holdBAfterRecDubKeepsTheRecording() {                       // Review Focus 1
    fresh(); set(kParamAfterRec, 1);
    set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(187);
    bDown(); CHECK(eng().state(0) == ouro::TrackState::Overdubbing);    // closed into an overdub
    runN(188); CHECK(eng().state(0) == ouro::TrackState::Playing && eng().loopLength() > 0 && eng().undoDepth(0) == 0);
    bUp();
}
static void holdBOnAnArmedEmptyTrackKeepsTheArm() {
    fresh(); set(kParamAutoRec, 1); bDown(); runN(188); CHECK(eng().armedTrack() == 0); bUp();
}
static void redoAfterHoldsBringsBackOnlyTheLast() {
    fresh(); recordBaseAndDub(); dubB(40);                              // depth 2
    bDown(); runN(188); bUp(); runN(10); bDown(); runN(188); bUp(); runN(10);
    CHECK(eng().undoDepth(0) == 0);
    set(kParamRedo, 1); runN(1); set(kParamRedo, 0); runN(10); CHECK(eng().undoDepth(0) == 1);
    set(kParamRedo, 1); runN(1); set(kParamRedo, 0); runN(10); CHECK(eng().undoDepth(0) == 1 && !eng().canRedo(0));
}
```

  Also: `holdBFromStoppedStartsThenUndoes` (after `recordBaseAndDub()` and a stop: `bDown()` → Running at the press;
  `runN(188)` → `undoDepth(0) == 0`, still Running); `holdBDroppedOnPartChange` (Hold Time 2000 ms; a hold on B whose press is before a queued part switch
  (`set(kParamPart, 1)`) and whose hold time falls after it: `undoDepth` in the new part is unchanged); in `layout()`
  `CHECK(!P().isParameterTrigger(kParamRec) && !P().isParameterTrigger(kParamStopClear))` and the symbol list's second
  entry `"stop_clear"`; in `tests/lv2/test_lv2_hooks.cpp` a check that the TTL block of `rec` and of `stop_clear` each
  contains `mod:preferMomentaryOnByDefault`.

- [ ] **Step 2: Run `make test` and `make test-lv2`.** Expected: compile errors (`kParamStopClear`) and failing B-hold
  checks.
- [ ] **Step 3: Implement.** Rename the enum entry and the `defs` row (`{"stop_clear", "Stop/Clear", Bool, 0, 1, 0,
  ""}`); `rec` becomes `{"rec", "Rec/Undo", Bool, 0, 1, 0, ""}`; the Makefile list becomes `for sym in rec stop_clear`.
  B in `run()`: on its rising edge `pressRec(sel)` as today; remember the track and `fEngine.part()` at the press and
  count held samples; when the count first reaches Hold Time, call `fEngine.peel(track)` unless the part changed
  since the press. One peel per hold. Presses during a load are already dropped (`droppedDuringLoad`). C's existing
  logic keeps working under the new name until Task 3.
- [ ] **Step 4: Run `make test` and `make test-lv2`.** Expected: both pass.
- [ ] **Step 5: Commit**: `plugin: holding B undoes a layer; rec and stop_clear are momentary`.

### Task 3: C while the loops run

**Files:**
- Modify: `plugins/Ouroboros/OuroborosPlugin.hpp` (replace the `// C footswitch:` block in `run()` and its members
  `fHeld`, `fDone`, `fTapPending`, `fStoppedPress`, `fTapStopped`, `fTapCancels`, `fTapTrack`, `fHold`, `fSinceTap`;
  the "B after a tap" block above `rising(kParamRec)`; the load reset line `fTapPending = false; fHeld = ...`)
- Modify: `tests/test_plugin.cpp` (replace `tapUndoHoldRedo`, `tapUndoWaitsForTheWindow`, `doubleTapThenHoldDoesNotClear`,
  `bAfterATapUndoesFirst`, `tapUndoesTheTrackItWasTappedOn`, `tapCancelsAWaitingActionAtOnce`,
  `tapCancelsAWaitOnAnotherTrack`; adapt `doubleTapStops`, `doubleTapBegunWhileStoppingOnlyStops`,
  `doubleTapDuringAShortFirstRecording` and the C presses in `syncPressWaitsForTheBar` to the new rules)

**Interfaces:**
- Consumes: `cancelWait`, `closeTake` (Task 1); `kParamStopClear`, `cDown`/`cUp`/`tapC` (Task 2); existing
  `toggleOn`/`setOn`, `clearTrack`, `stopNow()`, `request(kParamOn1 + t, v)` (On ports follow every on/off change).
- Produces: the gesture state Task 4 extends: the first press's track, part and transport; whether the first tap turned
  its track off or on (for the double-tap to put back); the tap-pending window.

The rules (spec, "C: Stop/Clear", running part), applied to the track `t` and part selected at the first press:

| At the first press | Press | Release before Hold Time | At Hold Time |
|---|---|---|---|
| `cancelWait(t)` returns true | nothing else | nothing | nothing |
| t is the first recording (`state == Recording && loopLength() == 0`) | nothing | tap pending; the window passing aborts it (`clearTrack(t)`) | `clearTrack(t)`, set on |
| t empty | nothing | nothing | nothing |
| t records onto the loop or overdubs | `closeTake(t)`, then off | — | `clearTrack(t)`, set on |
| t on | off at once | — | `clearTrack(t)`, set on |
| t off | nothing | on | `clearTrack(t)`, set on |

  A second press within the Double-Tap window after a released first press is a double-tap: it puts back what the
  first tap did, then `stopNow()` (during an End of loop or Fade stop that stops at once, as today). After a
  first-recording first tap, the double-tap is today's (keep it as the loop, stop silently), with no abort. A part
  change between the first press and any later step drops the gesture; a second press then counts as a new first
  press. A B press while a first-recording tap is pending applies the abort first (Rulings).

- [ ] **Step 1: Write the failing tests** (track 2 recorded onto the loop by `set(kParamTrack, 2)` and a B tap pair of
  `recordLoop()`'s length where a test needs a second track):

```cpp
static void cPressMutesAndReleaseUnmutes() {
    fresh(); recordBaseAndDub();
    cDown(); CHECK(!eng().isOn(0) && hasReq(kParamOn1, 0.0f)); runN(19); cUp(); runN(160);
    CHECK(!eng().isOn(0));                                              // stays off
    cDown(); CHECK(!eng().isOn(0)); runN(19); cUp(); CHECK(eng().isOn(0)); // on at the release
}
static void cHoldClearsTheTrackAndSetsItOn() {
    fresh(); recordBaseAndDub();
    cDown(); runN(186); CHECK(!eng().isOn(0) && eng().state(0) == ouro::TrackState::Playing);
    runN(2); CHECK(eng().state(0) == ouro::TrackState::Empty && eng().isOn(0)); cUp();
}
static void cHoldOnAnOffTrackNeverSounds() {
    fresh(); recordBaseAndDub(); tapC(); runN(160); CHECK(!eng().isOn(0));
    cDown(); for (int i = 0; i < 186; ++i) { runN(1); CHECK(!eng().isOn(0)); }
    runN(2); CHECK(eng().state(0) == ouro::TrackState::Empty && eng().isOn(0)); cUp();
}
static void cDoubleTapRestoresThenStops() {
    fresh(); recordBaseAndDub(); tapC(); runN(29); cDown();
    CHECK(eng().isOn(0) && eng().transport() != ouro::Transport::Running); cUp();
}
static void cDoubleTapRestoresBeforeAFadeStop() {                       // Review Focus 3
    fresh(); set(kParamStopMode, 2); recordBaseAndDub(); tapC(); runN(29); cDown();
    CHECK(eng().isOn(0) && eng().transport() == ouro::Transport::Stopping); cUp();
}
static void cTapOnAnEmptyTrackDoesNothing() {
    fresh(); recordBaseAndDub(); set(kParamTrack, 2); runN(1);
    tapC(); runN(160); CHECK(eng().isOn(1));
    tapC(); runN(29); cDown(); CHECK(eng().transport() != ouro::Transport::Running); cUp();  // a double-tap still stops
}
static void cDuringAnOverdubClosesItAndMutes() {
    fresh(); recordBaseAndDub(); set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(60);
    cDown(); CHECK(eng().state(0) == ouro::TrackState::Playing && eng().undoDepth(0) == 2 && !eng().isOn(0)); cUp();
}
static void cCancelsOnlyTheSelectedTracksArm() {
    fresh(); recordBaseAndDub(); set(kParamAutoRec, 1); set(kParamTrack, 3); runN(1);
    set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(1); CHECK(eng().armedTrack() == 2);
    set(kParamTrack, 1); runN(1); tapC(); CHECK(eng().armedTrack() == 2 && !eng().isOn(0));
    runN(160); set(kParamTrack, 3); runN(1); tapC(); CHECK(eng().armedTrack() == -1 && eng().isOn(2));
}
static void cDuringTheFirstRecording() {
    fresh(); set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(100);
    tapC(); CHECK(eng().state(0) == ouro::TrackState::Recording);       // the tap waits out the window
    runN(160); CHECK(eng().state(0) == ouro::TrackState::Empty);
    fresh(); set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(100);
    cDown(); runN(188); CHECK(eng().state(0) == ouro::TrackState::Empty); cUp();   // a hold aborts it
    fresh(); set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(100);
    tapC(); runN(10); set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(1);    // B while the abort is pending
    CHECK(eng().state(0) == ouro::TrackState::Recording && eng().loopLength() == 0);
}
```

  Also: `cGestureKeepsTheTrackItWasPressedOn` (Review Focus 2: tracks 1 and 2 recorded; `cDown()` on track 1,
  `set(kParamTrack, 2)`, hold to 188 blocks: track 1 Empty, track 2 not Empty); `cGestureDroppedOnPartChange` (Review
  Focus 4: Hold Time 2000 ms, a short loop, `cDown()` then `set(kParamPart, 1)`; hold past the part end and past 750
  blocks: `part() == 1` and track 1 not Empty).

- [ ] **Step 2: Run `make test`.** Expected: the new tests fail (C still undoes).
- [ ] **Step 3: Implement** the table and the double-tap rule in `run()`, replacing the old C block. Keep the existing
  block-count timing and the 'presses ignored during a load' reset. Every on/off change requests its `kParamOn1 + t`
  port, as `rising(kParamOnOff)` does. Transport Stopped is Task 4: until then a press while stopped does nothing.
- [ ] **Step 4: Run `make test` and `make test-lv2`.** Expected: both pass.
- [ ] **Step 5: Commit**: `plugin: C turns the selected track off and on, clears it on a hold`.

### Task 4: C while stopped

**Files:**
- Modify: `plugins/Ouroboros/OuroborosPlugin.hpp` (the C block from Task 3)
- Modify: `tests/test_plugin.cpp` (replace `tapWhileStoppedUndoes`, adapt `holdWhileStoppedClearsAll` and
  `doubleTapWhileStoppedResumes`; new tests)

**Interfaces:**
- Consumes: the Task 3 gesture state; `restart()`, `stop(StopMode::Instant, 0)`, `clearTrack`, `clearAll`, `setOn`,
  `userWaiting()`, `transport()`.
- Produces: nothing new for later tasks.

The rules (spec, "C: Stop/Clear", stopped part):

- **Tap** (released before Hold Time, selected track not empty): remember the current part's four on flags; every other
  track that holds audio goes off (Rulings), t on, On ports follow; `restart()`. Nothing happens on the press.
- **Double-tap** (second press within the window after such a tap): the remembered flags come back; if the restart
  still waits for its bar line (`userWaiting()`), it is left waiting, so everything starts on the line; if playback
  already started, everything restarts from the top (`stop(StopMode::Instant, 0)` then `restart()`, which the engine
  queues to the park).
- **Hold:** at Hold Time `clearTrack(t)` (set on); at twice Hold Time `clearAll()`. Nothing starts, and the release
  starts nothing.
- **Sync wait cancelled:** after a tap whose restart waits, if the loops are still stopped and no restart waits any more
  (B or a C press cancelled it), the remembered flags come back.

- [ ] **Step 1: Write the failing tests** (two recorded tracks, then `press(kParamStopPlay); CHECK(runUntil(stopped));`):

```cpp
static void cTapWhileStoppedStartsTheSelectedTrackAlone() {
    twoTracksStopped(); set(kParamTrack, 2); runN(1);
    cDown(); CHECK(stopped()); runN(19); cUp();
    CHECK(eng().transport() == ouro::Transport::Running && eng().isOn(1) && !eng().isOn(0) && hasReq(kParamOn1, 0.0f));
}
static void cSoloStartLeavesEmptyTracksOn() {
    twoTracksStopped(); tapC(); CHECK(eng().isOn(2) && eng().isOn(3));
}
static void cDoubleTapWhileStoppedStartsEverything() {
    twoTracksStopped(); set(kParamTrack, 2); runN(1); tapC(); runN(10); cDown(); cUp(); runN(60);
    CHECK(eng().transport() == ouro::Transport::Running && eng().isOn(0) && eng().isOn(1));
    CHECK(eng().position() < 128 * 80);                                 // restarted from the top at the second press
}
static void cHoldWhileStoppedClearsTheTrackThenEverything() {
    twoTracksStopped(); set(kParamTrack, 2); runN(1);
    cDown(); runN(188); CHECK(eng().state(1) == ouro::TrackState::Empty && eng().state(0) != ouro::TrackState::Empty && stopped());
    runN(188); CHECK(eng().empty() && eng().loopLength() == 0 && stopped());
    cUp(); runN(10); CHECK(stopped());
}
static void cTapWhileStoppedOnAnEmptyTrackDoesNothing() {
    twoTracksStopped(); set(kParamTrack, 3); runN(1); tapC(); runN(160); CHECK(stopped());
}
static void cSoloStartCountsAsAChange() {                               // Review Focus 5
    freshPieces(); twoTracksStoppedNoFresh(); saveTo(2);
    set(kParamTrack, 2); runN(1); tapC(); runN(160);
    press(kParamStopPlay); CHECK(runUntil(stopped));
    press(kParamLoad); CHECK(status() == "LOAD 2?");
}
```

  Also `cSyncWaitPutsTracksBackWhenCancelled`, with Sync on and MOD playing (`runMod`, as `syncPressWaitsForTheBar`):
  stopped, a tap on track 2 → `userWaiting()` and track 1 off; a B press → `!userWaiting()`, `stopped()`, both tracks
  on. The same with a C press after the window instead of B (`cancelWait` takes the pending restart): `!userWaiting()`,
  `stopped()`, both tracks on. Then a tap and a second press within the window during the wait → both tracks on and `userWaiting()` still true;
  run to the line → Running with both on. (`twoTracksStopped()` and `twoTracksStoppedNoFresh()` are this task's
  helpers: two tracks recorded, stopped; the second skips `fresh()`.)

- [ ] **Step 2: Run `make test`.** Expected: the new tests fail.
- [ ] **Step 3: Implement** the stopped rules in the C block.
- [ ] **Step 4: Run `make test` and `make test-lv2`.** Expected: both pass.
- [ ] **Step 5: Commit**: `plugin: C from a stop starts the selected track alone; a double-tap starts everything`.

### Task 5: Labels on the Dwarf

**Files:**
- Modify: `plugins/Ouroboros/hmi/HmiBridge.hpp`, `plugins/Ouroboros/OuroborosPlugin.hpp` (`hmiPort()`, the
  `fHmi.update({...})` call)
- Modify: `tests/test_hmi.cpp` (the `view()` helper; replace `undoLabelShowsAvailability`)

**Interfaces:**
- Produces: `enum HmiPort : uint8_t { kHmiRec, kHmiStop, kHmiOnOff, kHmiVolume, kHmiRedo, kHmiPortCount };`
  `struct HmiView { TrackState state; NextAction next; bool canRedo; int selected; bool on; Transport transport; bool
  armed; bool waiting = false; };` (`canUndo` replaced by `canRedo`). `hmiPort()`: `kParamStopClear` → `kHmiStop`,
  `kParamRedo` → `kHmiRedo`; `kParamRec`, `kParamOnOff`, `kParamVolume` as today.

- [ ] **Step 1: Write the failing tests** in `tests/test_hmi.cpp` (`view(s, n, redo, sel, on, tp, ...)`):

```cpp
static void stopLabelFollowsTheSelectedTrack() {
    gCalls.clear(); ouro::HmiBridge b; b.setFeature(&gFake); b.addressed(ouro::kHmiStop, A1, kAllCaps);
    b.update(view(ouro::TrackState::Playing, ouro::NextAction::Dub, false, 1, true));  CHECK(lastLabel(A1) == "ON 2");
    b.update(view(ouro::TrackState::Playing, ouro::NextAction::Dub, false, 1, false)); CHECK(lastLabel(A1) == "OFF 2");
    b.update(view(ouro::TrackState::Playing, ouro::NextAction::Dub, false, 1, true, ouro::Transport::Stopped));
    CHECK(lastLabel(A1) == "PLAY 2");
    b.update(view(ouro::TrackState::Empty, ouro::NextAction::Rec, false, 1, true)); CHECK(lastLabel(A1) == "-");
    CHECK(count("led") == 0);
}
static void redoLabelShowsAvailability() {
    gCalls.clear(); ouro::HmiBridge b; b.setFeature(&gFake); b.addressed(ouro::kHmiRedo, A1, kAllCaps);
    b.update(view(ouro::TrackState::Playing, ouro::NextAction::Dub, false)); CHECK(lastLabel(A1) == "-");
    b.update(view(ouro::TrackState::Playing, ouro::NextAction::Dub, true));  CHECK(lastLabel(A1) == "REDO");
}
```

- [ ] **Step 2: Run `make test`.** Expected: compile errors (`kHmiStop`).
- [ ] **Step 3: Implement** the labels (fingerprint and `send()` cases for `kHmiStop` and `kHmiRedo`; `sent_` sized for
  the new count) and pass `fEngine.canRedo(sel)` in `fHmi.update`.
- [ ] **Step 4: Run `make test` and `make test-lv2`.** Expected: both pass.
- [ ] **Step 5: Commit**: `hmi: C shows the selected track's state; knob 2 shows REDO`.

### Task 6: Manual and README

**Files:**
- Modify: `docs/manual/ouroboros-manual.html`, then regenerate `docs/manual/ouroboros-manual.pdf` with `make manual`
- Modify: `README.md` (feature list line on footswitches, if it names Undo on C)
- Modify: `tests/lv2/test_lv2_hooks.cpp` comment that mentions `undo_redo`

- [ ] **Step 1: Rewrite** in the manual, matching the spec's Behaviour section:
  - Quick start: undo is "hold B"; a tap on C turns the selected track off and on.
  - The controls table: knob 2 press is Redo (assign Redo to it in MOD-UI); knobs 1 and 3 unchanged.
  - "Footswitch B (REC)": the tap table unchanged; a Hold row (undo one layer per hold, never the first; what the hold
    does after a press that started or closed an overdub, a recording, a wait, an arm, a start from stopped); Redo
    after holds brings back only the last hold's layer.
  - "Footswitch C (UNDO)" becomes "Footswitch C (STOP/CLEAR)": the running table (press off, release on, hold clears and
    sets on, double-tap puts back and stops, empty track), the stopped table (tap starts the selected track alone and
    turns off the other tracks with audio, double-tap starts everything, hold clears the track then everything), the
    first recording (tap aborts after the window, double-tap keeps and stops, hold aborts), a take on the selected
    track (closed and turned off), waits and arms (only the selected track's, or a pending start), a gesture belongs
    to the track and part it was pressed on.
  - Settings: Hold Time ("how long to hold B or C"; twice it on C while stopped clears everything); Double-Tap ("the
    window for a second press on C"); Stop Mode unchanged; Auto Rec ("An arm is cancelled by B on the armed track, a
    press on C with that track selected, ..."); Sync's WAIT ("Press B again, or press C with that track selected").
  - Saved pieces: "B or a double-tap on C starts everything; a tap on C starts the selected track alone".
  - The labels: C shows `ON n` / `OFF n`, `PLAY n` while stopped, `-` for an empty track; knob 2 `REDO` / `-`.
- [ ] **Step 2: Run `make manual`, `make test`, `make test-lv2`.** Expected: the PDF is written; both test runs pass.
- [ ] **Step 3: Commit**: `docs: manual: hold B to undo, C stops and clears, Redo on knob 2`.
