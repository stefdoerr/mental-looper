# B double-tap: a new part from here — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A double-tap on B goes to the next part; an empty next part keeps the tracks below the selected one and records the selected track from its first beat.

**Architecture:** The engine (`ouro::Looper`) gets one call, `newPart(t)`, which queues the next part and, at the
switch sample, copies only the tracks below `t` and starts `t`'s recording (stopped: at the start that follows). The
plugin's `run()` turns B's presses into a double-tap, undoes what the first press did, and calls `newPart`. The HMI and
the face show the wait. No new port.

**Tech Stack:** C++14 (gnu++14), DPF/LV2, header-only engine; tests are plain `main()` programs under ASan (`make test`),
plus `make test-lv2`, `make test-modgui`, `make manual`.

**Spec:** `docs/superpowers/specs/2026-11-26-new-part-double-tap-design.md` (approved). Read it before your task; the
tables there are binding.

## Global Constraints

- B's single tap acts on the press exactly as today; nothing may delay it (the spec's first anti-goal).
- No new port; the port order and symbols are frozen since v0.1.0 (`tests/test_plugin.cpp` and
  `tests/lv2/test_lv2_hooks.cpp` already pin the count at 82).
- Double-Tap window = the existing Double-Tap setting (150–600 ms, default 400 ms), timed in whole blocks as C's is
  (counted from the first press's release).
- Hold Time = the existing setting; a first press held past it is B's undo (`peel`) and opens no double-tap window.
- The audio loop gains no per-sample work: the engine acts only at a part switch, a start, or a call. CPU rule (user):
  "it should not add significant CPU cost to our plugin especially when not used".
- Any DPF change stays in `patches/dpf-lv2-hooks.patch` (none expected).
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Do not push.

## Review Focus

1. A double-tap whose first press was a one-press handover: the closed take stays complete (its length and audio),
   and the aborted new start leaves nothing behind — test in Task 3 (`bDoubleTapAfterAHandoverKeepsTheClosedTake`).
2. The switch sample: the new recording's first sample is the new part's first sample — test in Task 1
   (`newPartRecordsFromTheSwitchSample`).
3. Long copies below: the new part's pass is its longest track and grows when the new recording closes longer — test
   in Task 1 (`newPartPassFollowsALongCopy`).
4. Memory full at the switch: the recording closes at once (FULL), the copies still play — test in Task 1
   (`newPartWithMemoryFullStillCopies`).
5. A Load while the new part waits drops it — test in Task 3 (`aLoadDropsTheWaitingNewPart`).

(The spec's 6th, a Sync double-tap on the first recording, is Task 2's `newPartAfterAWaitingCloseSwitchesAtThatPassEnd`.)

---

### Task 1: Engine — `newPart` while running, the switch step, cancels

**Files:**
- Modify: `plugins/MentalLooper/dsp/Looper.hpp` (`setPart`, `switchPart`, `copy` neighbourhood, `pressRec`,
  `cancelWait`, `stop`, `clearAll`, `recWaiting`, the load/import reset, private members)
- Test: `tests/test_parts.cpp`

**Interfaces:**
- Produces:
  - `void newPart(int t)` — public. Next part = `(part() + 1) % kParts`. Turns Song off (as `setPart` does). If the next
    part is empty (every track `Empty`, `switchPart`'s `fill` test) it remembers `t` as the track to record; else it
    queues a plain switch exactly as `setPart(next)`. While running it queues (`queuedPart()` = next); during the first
    recording the switch waits for the first part end after its close, as `setPart` does. Stopped / Stopping / nothing
    recorded: Task 2 (in this task those paths may simply fall through to `setPart`-like behaviour).
  - `int newPartTrack() const` — the track a waiting new part will record, -1 if none.
  - `switchPart(p)` with a remembered track `t` and an empty `p`: tracks `u < t` copied with `copy()`; tracks `u >= t`
    get only on/off and volume from the part left (what `copy()` gives an empty source); track `t` set on; then `t`
    starts a recording onto the loop on this sample (`startDub(..., D = 0, ..., base = true, ...)`, with `cont`,
    `offset`, `trim` taken from the closes in `switchPart` the way `pressRec` takes them from a handover), or, with
    Auto Rec on, `armed_ = t`. The remembered track is cleared by every switch.
  - Cancels: `pressRec(any t)` while a new part waits cancels it (queue and track) and returns, doing nothing else.
    `cancelWait(any t)` also cancels it and returns true. `setPart(p)` forgets the track (a knob switch replaces it,
    the current part cancels it, as today). `stop(...)` cancels it when the stop begins (a knob-queued switch still
    happens at the park). `clearAll()` and the piece load (`importEnd` path) clear it.
  - `recWaiting()` is also true while `newPartTrack() >= 0` (the face's waiting look). `userWaiting()` and
    `userWaiting(t)` do not change (the Loop widget keeps showing `A>B`, and C's restart logic reads `userWaiting()`).

- [ ] **Step 1: Write the failing tests** in `tests/test_parts.cpp` (call each from `main`):

```cpp
// Tracks 0-2 hold audio in A; newPart(2): at A's end, B has 0-1 copied, 2 recording, 3 empty.
static void newPartCopiesBelowAndRecordsTheSelectedTrack();
//   CHECK(queuedPart()==1 && newPartTrack()==2 && part()==0) right after newPart(2);
//   one sample before the end: part()==0; after it: part()==1, state(0)==Playing, state(1)==Playing,
//   state(2)==Recording, state(3)==Empty, newPartTrack()==-1; B after one more loop: trackLength(2)==24000.
static void newPartRecordsFromTheSwitchSample();          // Review Focus 2
//   feed silence up to the switch and a 1.0 impulse on part B's first sample; after closing at one loop, track 2
//   heard alone has the impulse at trackPosition 0 and nothing before it (|x| < 1e-6 elsewhere).
static void newPartAboveTheSelectedTrackIsSilent();
//   A: track 0 sine, track 1 a loud other sine; newPart(1), silence fed: after the switch's fade-in the output equals
//   track 0 alone (maxAbsDiff < 1e-6 against a capture of track 0 alone made in A).
static void newPartTakesOnAndVolumeFromThePartLeft();
//   A: volume(0)=-6 dB; track 1 empty, off, -12 dB; track 2 off. newPart(2) -> B: volumeDb(0)==-6, !isOn(1),
//   volumeDb(1)==-12, isOn(2) (forced on), volumeDb(2) == A's.
static void newPartToAPartWithAudioIsAPlainSwitch();
//   B already recorded; from A, newPart(2): newPartTrack()==-1, queuedPart()==1; at the switch B's tracks are unchanged
//   (same layerCount and trackLength) and nothing records.
static void newPartWrapsFromDToA();                       // in part 3, newPart -> queuedPart()==0
static void aTapOnBCancelsTheNewPart();
//   newPart(2); pressRec(0): queuedPart()==-1, newPartTrack()==-1, state(0)==Playing (no overdub started).
static void cancelWaitCancelsTheNewPartOnAnyTrack();      // newPart(2); CHECK(cancelWait(0) && newPartTrack()==-1)
static void partKnobStopAndClearAllDropTheNewPart();
//   newPart(2); setPart(3): queuedPart()==3, newPartTrack()==-1. newPart(2); setPart(0): queuedPart()==-1.
//   newPart(2); stop(Instant): newPartTrack()==-1 at once, and after the park part()==0.
//   newPart(2); clearAll(): newPartTrack()==-1.
static void newPartTurnsTheSongOff();                     // setSong(true); newPart(1): !songOn()
static void newPartArmsWithAutoRec();                     // setAutoRec(true); newPart(1); after the switch:
                                                          // armedTrack()==1 && state(1)==Empty
static void newPartPassFollowsALongCopy();                // Review Focus 3: track 0 two loops long; newPart(1);
//   after the switch partLength()==2L; track 1 closed after 2.6 loops -> partLength()==3L.
static void newPartWithMemoryFullStillCopies();           // Review Focus 4: a Rig with a pool that fills during
//   track 1's recording: memoryFull(), state(1)==Playing (closed as at the limit), state(0)==Playing.
static void recWaitingWhileANewPartWaits();               // newPart(1): recWaiting() && !userWaiting()
```

- [ ] **Step 2: Run to verify they fail**

Run: `make test 2>&1 | grep -m5 -E "error|FAIL"`
Expected: compile error, `newPart` / `newPartTrack` not declared.

- [ ] **Step 3: Implement** `newPart`, `newPartTrack`, the switch step and the cancels in `Looper.hpp` as the
Interfaces block says. One private `int newRec_ = -1;` next to `queued_`. Keep `switchPart`'s existing order: closes,
arm and waits dropped, then the fill loop (now partial), then the start of `t`'s recording after the loop. Keep
`changes_ += fill` (the spec: the copies and the recording count as changes for saved pieces).

- [ ] **Step 4: Run all tests**

Run: `make test 2>&1 | tail -3`
Expected: `==> all tests passed` (every existing parts test unchanged).

- [ ] **Step 5: Commit**

```bash
git add plugins/MentalLooper/dsp/Looper.hpp tests/test_parts.cpp
git commit -m "engine: newPart, the next part from the selected track (copies below, records it at the switch)"
```

---

### Task 2: Engine — stopped, stopping, nothing recorded, Sync's waiting close

**Files:**
- Modify: `plugins/MentalLooper/dsp/Looper.hpp` (`newPart`, `switchPart`, `start`, `park` path, `pressRec`'s and
  `cancelWait`'s restart cancel, `stop`, the pending-action firing in `process`, `Pending`)
- Test: `tests/test_parts.cpp`

**Interfaces:**
- Consumes: Task 1's `newPart`, `newPartTrack`, the partial switch.
- Produces:
  - `newPart(t)` while **Stopped**: switches at once with the partial copy (as at a running switch, minus the
    recording), remembers `t` as the track to record at the start, then calls `restart()` (which may wait for a bar
    line with Sync). `start()` begins `t`'s recording onto the loop with `D = since` (the start's own back-date), or
    arms it with Auto Rec. A cancelled pending restart (a B press or `cancelWait`), `stop()`, `clearAll()` or a load
    forget it: the looper stays stopped in the new part, nothing records. `newPartTrack()` reports it while it waits.
  - `newPart(t)` while **Stopping**: queues the next part for the park (the park's queued switch does the partial
    copy) and queues a restart (`restartQueued_`), so the park starts the new part with `t` recording. This is the
    plugin's path when the double-tap's first press had started everything from stopped (it stops Instant first).
  - `newPart(t)` with **nothing recorded** (`loopLength()==0`, no first recording): switches at once (nothing to
    copy), then `pressRec(t)` (first recording, or arm, or Sync wait, as B's tap).
  - `void dropStart(int t)` — public, for the plugin's second press: cancels `t`'s arm and a waiting press that would
    **start** a recording; a waiting press that would **close the first recording** stays, and a waiting handover
    (a press on another track while the first recording waits to close) becomes a close only. The close then fires
    without Rec > Dub. Implement it as a new `Pending::Close` kind that fires `close(recTrack_, false)` on the line;
    `userWaiting`, `recWaiting` and `cancelWaiting` treat `Close` as they treat `Press` on the first recording's track.

- [ ] **Step 1: Write the failing tests** in `tests/test_parts.cpp` (Sync ones use `SyncRig` from `looper_rig.hpp`):

```cpp
static void newPartWhileStoppedSwitchesAndStartsRecording();
//   loop on track 0; stop(Instant) and let it park; newPart(1): part()==1, transport()==Running, state(0)==Playing,
//   state(1)==Recording, newPartTrack()==-1.
static void newPartWhileStoppingStartsAtThePark();
//   stop(Instant); newPart(1) at once (Stopping): after the park part()==1, Running, state(1)==Recording.
static void newPartWhileStoppedWithSyncWaitsForTheLine();
//   SyncRig, stopped mid-bar: newPart(1): part()==1, stopped, newPartTrack()==1; at the line: Running,
//   state(1)==Recording. Again from scratch, then pressRec(0) before the line: newPartTrack()==-1, part()==1,
//   stopped, state(1)==Empty.
static void newPartWithNothingRecordedMovesAndRecords();
//   fresh Rig: newPart(0): part()==1, state(0)==Recording, loopLength()==0.
static void dropStartKeepsAWaitingCloseOfTheFirstRecording();
//   SyncRig, setRecToDub(true), first recording on 0, a closing press that waits: dropStart(0): userWaiting(0) still;
//   at the line loopLength()>0 and state(0)==Playing (no overdub).
static void dropStartTurnsAWaitingHandoverIntoAClose();
//   first recording on 0 waiting... pressRec(1) waits as a handover; dropStart(1): at the line state(0)==Playing,
//   state(1)==Empty.
static void dropStartCancelsAWaitingStartAndAnArm();
//   SyncRig with a loop: pressRec(1) on empty waits; dropStart(1): !userWaiting(). Auto Rec: pressRec(2) arms;
//   dropStart(2): armedTrack()==-1.
static void newPartAfterAWaitingCloseSwitchesAtThatPassEnd();   // the spec's Review Focus 6
//   SyncRig, first recording on 0, closing press waits, dropStart(0), newPart(1): at the line the loop closes; at the
//   end of that first pass part()==1, state(0)==Playing (copy), state(1)==Recording.
```

- [ ] **Step 2: Run to verify they fail**

Run: `make test 2>&1 | grep -m5 -E "error|FAIL"`
Expected: `dropStart` not declared; the stopped/stopping tests FAIL on `part()` or `state(1)`.

- [ ] **Step 3: Implement** the paths in the Interfaces block. One private `int startRec_ = -1;` (the stopped path's
track), set by the stopped partial switch, read and cleared by `start()`.

- [ ] **Step 4: Run all tests**

Run: `make test 2>&1 | tail -3`
Expected: `==> all tests passed`.

- [ ] **Step 5: Commit**

```bash
git add plugins/MentalLooper/dsp/Looper.hpp tests/test_parts.cpp
git commit -m "engine: newPart while stopped, stopping or empty; dropStart keeps Sync's waiting close"
```

---

### Task 3: Plugin — B's double-tap

**Files:**
- Modify: `plugins/MentalLooper/MentalLooperPlugin.hpp` (`run()`: the B press block at `rising(kParamRec)` and the
  B hold block; C's press closes B's window; members next to `fBHold`)
- Test: `tests/test_plugin.cpp`

**Interfaces:**
- Consumes: `newPart(int)`, `newPartTrack()`, `dropStart(int)`, `undo(int)`, `cancelWait(int)`, `stop(StopMode)`.
- Produces: nothing new for later tasks.

Gesture rules (spec "The gesture"): the first press calls `pressRec(sel)` as today and is classified by comparing the
selected track and transport just before and just after that call:

| Before → after the first press | Kind | The second press does |
|---|---|---|
| `newPartTrack() >= 0` before (the press cancelled it) | nothing | — |
| transport Stopped before | started all | Running now: `stop(Instant)`; a restart waiting: `cancelWait(t)`; then as below |
| transport Stopping before | nothing (the stop was cancelled) | — |
| Playing → Overdubbing | started dub | `undo(t)` if still Overdubbing |
| Empty → Recording | started rec | `undo(t)` if still Recording (aborts) |
| Recording → Overdubbing (Rec > Dub) | closed to dub | `undo(t)` if still Overdubbing |
| an arm or a user wait on `t` appeared | wait | `dropStart(t)` |
| anything else (closed a take, cancelled a wait or arm) | nothing | — |

Then the second press calls `newPart(t)`. `t` and the part are those right after the first press. The window opens on
the first press's release if its hold did not fire, runs for the Double-Tap setting, and closes on: the second press,
its end, a C press, a part change (checked at the second press). The second press sets B's hold as done (holding it
does nothing). Its release opens no window (a third press is a first press).

- [ ] **Step 1: Write the failing tests** in `tests/test_plugin.cpp` (helpers next to `tapC`:
  `bDown()`/`bUp()` as `cDown`/`cUp` for `kParamRec`, `tapB()` = down, 4 blocks, up; `doubleTapB()` = `tapB()`,
  20 blocks, `bDown()`, `bUp()`):

```cpp
static void bDoubleTapOnAnEmptyTrackQueuesANewPartRecordingIt();
//   recordLoop(); Track 2; doubleTapB(): newPartTrack()==1, queuedPart()==1, state(1)==Empty;
//   runUntil(part()==1): state(0)==Playing, state(1)==Recording.
static void bDoubleTapOnAPlayingTrackCancelsTheOverdub();
//   recordLoop(); (Track 1) doubleTapB(): state(0)==Playing, undoDepth(0)==0, newPartTrack()==0.
static void bDoubleTapClosingATakeKeepsIt();
//   recordLoop(); tapB() (overdub), 40 blocks; doubleTapB(): undoDepth(0)==1, newPartTrack()==0.
static void bDoubleTapAfterAHandoverKeepsTheClosedTake();   // Review Focus 1
//   recordLoop(); Track 2, tapB(), 300 blocks (track 1 records past the loop); Track 3; doubleTapB():
//   state(1)==Playing, trackLength(1)==2 loops, state(2)==Empty, newPartTrack()==2.
static void bSecondPressHeldDoesNotUndo();
//   recordLoop(); tapB(); runN(40); tapB(); runN(40) (one overdub: undoDepth(0)==1); tapB(), 20 blocks, bDown(),
//   250 blocks held, bUp(): undoDepth(0)==1 (the first press's overdub was cancelled, the hold peeled nothing).
static void aFirstPressHeldIntoAnUndoStartsNoDoubleTap();
//   depth 1; hold B 250 blocks (peel), release, bDown() within 20 blocks: newPartTrack()==-1, state(0)==Overdubbing.
static void cBetweenBPressesMakesANewFirstPress();         // tapB(); cDown(); cUp(); bDown(): newPartTrack()==-1
static void bDoubleTapWithTrackTurnedRecordsTheFirstPressTrack();
//   recordLoop(); Track 2, tapB(), Track 3, 20 blocks, bDown(): newPartTrack()==1.
static void bDoubleTapWhileStoppedStartsTheNextPartRecording();
//   recordLoop(); press(kParamStopPlay) until stopped(); Track 2; doubleTapB(); runUntil(part()==1 && running):
//   state(1)==Recording, state(0)==Playing.
static void aTapOnBOrAPressOnCCancelsTheWaitingNewPart();
//   queue with doubleTapB(); 160 blocks; tapB(): newPartTrack()==-1, queuedPart()==-1. Queue again; cDown():
//   newPartTrack()==-1; cUp(); 20 blocks; cDown() (C's double-tap): runUntil(stopped) and part()==0.
static void bDoubleTapTurnsTheSongOff();                   // song({{0,1},{1,1}}); Song on; doubleTapB(): !songOn()
static void aLoadDropsTheWaitingNewPart();                 // Review Focus 5
//   saveTo(2) of a loop; doubleTapB() queues; press(kParamLoad) (confirm if asked) and waitPieces():
//   newPartTrack()==-1, queuedPart()==-1.
```

- [ ] **Step 2: Run to verify they fail**

Run: `make test 2>&1 | grep -m5 FAIL`
Expected: FAILs in the new tests (`newPartTrack()==-1` where a new part is expected).

- [ ] **Step 3: Implement** the gesture in `run()` as the rules above. Keep the existing "B after C's tap ends C's
window" lines; add the symmetric "C's press closes B's window".

- [ ] **Step 4: Run all tests**

Run: `make test 2>&1 | tail -3 && make test-lv2 2>&1 | tail -1`
Expected: `==> all tests passed` and `OK`.

- [ ] **Step 5: Commit**

```bash
git add plugins/MentalLooper/MentalLooperPlugin.hpp tests/test_plugin.cpp
git commit -m "plugin: B double-tap goes to the next part, recording the selected track there"
```

---

### Task 4: The wait on the Dwarf and the face

**Files:**
- Modify: `plugins/MentalLooper/hmi/HmiBridge.hpp` (`HmiView`, `fingerprint(kHmiRec)`, `send(kHmiRec)`),
  `plugins/MentalLooper/MentalLooperPlugin.hpp` (the `fHmi.update({...})` call)
- Test: `tests/test_hmi.cpp`, `tests/test_face.cpp`

**Interfaces:**
- Consumes: `newPartTrack()`; `recWaiting()` (Task 1 already includes the new part).
- Produces: `HmiView::newPart` — `bool newPart = false;` after `waiting`; the plugin passes
  `fEngine.newPartTrack() >= 0`.

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/test_hmi.cpp
static void recLabelAndLedWhileANewPartWaits();
//   a Playing view (label DUB) with newPart = true: label "REC", LED Red with Blink_Fast; the same view again sends
//   nothing; newPart = false: label "DUB" and the LED as before.
// tests/test_face.cpp
static void faceWaitsWhileANewPartWaits();
//   engine with a loop on track 0, newPart(1): faceView(e, 0).looper == kFaceWaiting; after the switch,
//   kFaceWriting.
```

- [ ] **Step 2: Run to verify they fail**

Run: `make test 2>&1 | grep -m5 -E "error|FAIL"`
Expected: `HmiView` has no member `newPart` (the face test may already pass through Task 1's `recWaiting`).

- [ ] **Step 3: Implement**: in `send(kHmiRec)` a waiting new part wins over the other label and LED rules (label
`REC`, red, `LV2_HMI_LED_Blink_Fast`); `fingerprint(kHmiRec)` includes the bit.

- [ ] **Step 4: Run all tests**

Run: `make test 2>&1 | tail -3 && make test-modgui 2>&1 | grep -v "^ok" | tail -3`
Expected: `==> all tests passed`; the modgui checks print no failure.

- [ ] **Step 5: Commit**

```bash
git add plugins/MentalLooper/hmi/HmiBridge.hpp plugins/MentalLooper/MentalLooperPlugin.hpp tests/test_hmi.cpp tests/test_face.cpp
git commit -m "hmi: B shows REC, blinking fast red, while a new part waits"
```

---

### Task 5: Manual and README

**Files:**
- Modify: `docs/manual/mental-looper-manual.html`, `docs/manual/mental-looper-manual.pdf` (generated), `README.md`

- [ ] **Step 1: Edit the manual** (the spec's "Manual" section), in the manual's existing voice:
  - Footswitch B: a `Double-tap` row in the B table (shows `REC`); a **New part** paragraph with the Success example
    (tracks 1–3 in A, track 3 selected, double-tap: B keeps 1–2, records 3 from its first beat); the second press's
    table (spec, "The second press first deals with what the first press did"); the wait (B blinks fast red, `A>B`,
    a tap on B or a press on C cancels; any stop cancels); while stopped; with nothing recorded.
  - Song parts, "Switching": by foot with B's double-tap; the layer rule (below copied, the selected track records,
    above empty); the Part knob still copies everything.
  - Settings, Double-Tap: "The window for a second press on B and on C."
- [ ] **Step 2: README**: in the "Parts and a song" bullet, add "Double-tap B to go to the next part, keeping the tracks
  below the selected one and recording it from the first beat."
- [ ] **Step 3: Regenerate and check**

Run: `make manual && python3 -c "import re;d=open('docs/manual/mental-looper-manual.pdf','rb').read();print(len(re.findall(rb'/Type\s*/Page[^s]',d)),'pages')"`
Expected: `==> docs/manual/mental-looper-manual.pdf` and a page count; open the PDF pages that changed and check no
table is split from its lead-in.

- [ ] **Step 4: Commit**

```bash
git add docs/manual/mental-looper-manual.html docs/manual/mental-looper-manual.pdf README.md
git commit -m "docs: B double-tap, a new part from here"
```

---

### Task 6: Whole-suite check and the Dwarf bench

- [ ] **Step 1:** `make test && make test-lv2 && make test-modgui && make dwarf-build` — all pass; `git status` clean.
- [ ] **Step 2: Bench on the Dwarf** when it is connected (`ssh root@192.168.51.1 true`): cross-compile
  `tests/bench/bench.cpp` for this branch and for main at the real flags (`-std=gnu++14 -O3 -ffast-math
  -fdata-sections -ffunction-sections -DNDEBUG -static-libstdc++ -static-libgcc` with
  `/root/mod-workdir/moddwarf-new/host/usr/bin/aarch64-modaudio-linux-gnu-g++` in the `moddwarf-cross` image), run
  them pinned and interleaved 5 rounds in the Dwarf's `/tmp` as the scratchpad's `fsbench/run.sh` does, and delete the
  binaries after. Expected: within noise of main. If the Dwarf is not connected, report that the bench is still owed.
