# Ouroboros Tempo Sync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** With Sync = Bars, recordings start (and the first loop closes) exactly on MOD's bar lines, every loop is whole bars, and the looper follows MOD's play/stop. With Sync = Off nothing changes.

**Architecture:**
- **Plugin side:** each block, the plugin turns DPF's `TimePosition` into a bar grid (valid, playing, samples per bar, samples to the next bar line) and hands it to the engine before `process()`. It also catches MOD's play and stop edges.
- **Engine side:** the engine owns the timing:
  - it holds a quantized action until the exact sample of the next line, or back-dates a late press to the last line through the input history;
  - it stores the first loop's bar count, so that loop can serve as the grid while MOD is stopped;
  - it retriggers every playhead on a line with a 3 ms crossfade when MOD starts.

**Tech Stack:** C++14, the header-only engine in `plugins/Ouroboros/dsp/`, DPF (LV2) with `DISTRHO_PLUGIN_WANT_TIMEPOS`, and the repo's `CHECK`/`REPORT` harness (`make test`, `make test-lv2`).

**Spec:** `docs/superpowers/specs/2026-11-24-ouroboros-tempo-sync-design.md`. It builds on the looper, stop/clear/auto-record and longer-tracks specs in the same folder.

## Global Constraints

- **Sync is a setting:** `sync` (enum Off/Bars, default Off), in Settings, after `double_tap` and before bypass. With Sync = Off, or with no valid grid, behaviour is bit-identical to today.
- **RT-safe:** `process()`, `run()` and every engine control are RT-safe: no allocation, no locks, no I/O.
- **Grace window:** a late press up to `kSyncGraceMs = 150` ms after a line counts as on it. `kHistoryMs` grows to 250. The back-date is the samples since the line, and it replaces the trigger latency for that press.
- **What snaps:** first-recording start, first-recording close, starting a recording onto the loop, an auto-record start, and a restart from stopped while MOD plays.
- **What stays immediate:** closing a recording onto the loop (the longer-tracks rule), overdub punch in/out, undo/redo/on/off/clear, and the double-tap stop.
- **Waiting actions:** a waiting action is cancelled by B again (same track) or by a tap on C. While it waits, B's LED blinks fast in the coming colour and the widget shows `WAIT`.
- **First loop length:** `bars = max(1, round(samples since its start line / samples per bar at the close))`, and `L = round(bars × samples per bar)`.
- **MOD play edge** (off→on, seen while Sync = Bars; the first block after load or after Sync turns on is not an edge):
  - every non-empty track retriggers on the line: the longest lands on its beginning, as at a park;
  - any recording or overdub closes first;
  - the switch uses a 3 ms crossfade;
  - from stopped it restarts, with the usual 3 ms fade-in;
  - a start mid-bar waits for the next line;
  - a first recording in progress ignores the edge.
- **MOD stop edge:** stop with the Stop Mode setting. During a first recording, it closes on the last line passed if at least one bar was recorded; otherwise it discards.
- **The grid while MOD is stopped:** before a loop exists, the first quantized press is bar 1 at MOD's tempo. After that, the loop's own bars are the grid (L / bars, lines from the loop top). A loop recorded with Sync Off counts as 1 bar.
- **Commits:** commit after every task on branch `worktree-tempo-sync`, ending each message with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **Sync turned on over loops recorded free:** no jump. The free loop counts as 1 bar, and B on an empty track waits for the loop top. (Test in Task 2.)
2. **A tempo change during the first recording:** L is whole bars at the tempo at the close. (Test in Task 1.)
3. **MOD play while a track overdubs:** the overdub closes as a layer, the retrigger is click-free, and undo still removes that layer exactly. (Test in Task 3.)
4. **MOD play mid-bar:** the retrigger waits for the next line. (Test in Task 3.)
5. **No valid grid with Sync = Bars:** presses act at once. A footswitch never goes dead. (Test in Task 1.)

---

### Task 1: Engine — bar grid and quantized first recording

**Files:**
- Modify: `plugins/Ouroboros/dsp/Constants.hpp` (`kHistoryMs = 250.0f`, add `kSyncGraceMs = 150.0f`) and `plugins/Ouroboros/dsp/Looper.hpp`
- Test: `tests/test_sync.cpp` (new)

**Interfaces:**
- Produces (public):
  - `void Looper::setSync(bool on);` (default off)
  - `void Looper::setGrid(bool valid, bool playing, double samplesPerBar, double toNextBar);` is called once per block, before `process()`. `toNextBar` is the number of samples from the block start to the next bar line, in `[0, samplesPerBar)`.
  - `bool Looper::waiting() const;` is true while a quantized action is pending.
  - `int Looper::waitingTrack() const;` gives that action's track, or −1 for a restart or for no action.
  - `uint32_t Looper::bars() const;` gives the first loop's bar count, or 0 when there is no loop.
- Internal:
  - The engine keeps its grid per sample within the block: samples since the last line and samples to the next line.
  - The grid source:
    - when `playing`, MOD's grid;
    - else, if `L_ > 0`, the loop's bars (line at `pos_ % (L_/bars_)` = 0, with `bars_ = 1` for a loop recorded without sync);
    - else, a free grid anchored at the first quantized press (that press is a line), at `samplesPerBar`.
  - `!valid` or `!sync` means no grid: everything behaves as today.

**Behaviour to implement (this task):**
- **First recording start:** `pressRec(t)` on an empty track, with L unset and a grid:
  - **Late press:** samples since the last line ≤ `msToSamples(kSyncGraceMs)` means start now, back-dated by that many samples (`startFirst(t, since)`).
  - **Early press:** otherwise the press waits for the next line, then starts with back-date 0. With the free grid (MOD stopped, no loop), the press itself is the line, so it starts now with back-date 0.
- **First recording close:** `pressRec` on the recording track, with a grid:
  - **Late press:** since ≤ grace means close now, back-dated by `since`;
  - **Early press:** otherwise it waits for the next line and closes there, back-dated 0.
  - **Length:** `bars = max(1, round(samples since the start line / samplesPerBar))` and `L = round(bars × samplesPerBar)`.
  - **`closeFirst` signature:** it takes the close back-date (in place of `recD_`) and the loop length explicitly. Without a tempo change, L equals the recorded span to the line. After a tempo change it can differ: anything recorded past L is its tail, as today.
  - **Back-dates longer than the history:** its `histUsed_` loop covers only `min(back, histLen_)` samples. A back-date can exceed the history (Task 3 closes on the last line).
- **Cancel:** a second `pressRec` on the same track while its action waits cancels it. So does `undo(t)` (a tap on C), which then does nothing else.
- **No grid:** all of the above act exactly as today. Sync Off must be bit-identical.

- [ ] **Step 1: Write the failing tests** in `tests/test_sync.cpp`:

```cpp
#include "looper_rig.hpp"
using ouro::TrackState;

// Drives a Looper with a virtual MOD transport: before each block it hands the engine the grid at that block start.
struct SyncRig : Rig {
    bool valid = true, playing = true; double spb = 96000.0, phase = 0.0;   // 120 BPM 4/4 at 48 kHz; phase: samples into the bar
    using Rig::Rig;
    void feedSync(const std::vector<float>& in) {
        std::vector<float> o(block);
        for (size_t i = 0; i < in.size(); i += block) {
            const uint32_t n = uint32_t(std::min<size_t>(block, in.size() - i));
            lp.setGrid(valid, playing, spb, phase == 0.0 ? 0.0 : spb - phase);
            lp.process(in.data() + i, o.data(), n);
            out.insert(out.end(), o.begin(), o.begin() + n);
            if (playing) phase = std::fmod(phase + n, spb);
        }
    }
    void silenceSync(uint32_t n) { feedSync(std::vector<float>(n, 0.0f)); }
    void toPhase(double p) { silenceSync(uint32_t(std::fmod(p - phase + 2 * spb, spb))); }   // block-quantized is fine at 128 | p
};

static void earlyPressWaitsForTheLine() {
    SyncRig r; r.lp.setSync(true); r.toPhase(96000 - 1280);              // 10 blocks before the line
    r.lp.pressRec(0); CHECK(r.lp.waiting() && r.lp.state(0) == TrackState::Empty);
    r.silenceSync(1280 + 128);                                          // the line opens the 11th block
    CHECK(!r.lp.waiting() && r.lp.state(0) == TrackState::Recording);
    CHECK(r.lp.recordedSamples() == 128);                               // started on the line
}
static void latePressIsBackDatedToTheLine() {
    SyncRig r; r.lp.setSync(true); r.toPhase(1280);                      // 26.7 ms after the line
    r.lp.pressRec(0); CHECK(!r.lp.waiting() && r.lp.state(0) == TrackState::Recording);
    CHECK(r.lp.recordedSamples() == 1280);                                // back-dated exactly to the line
}
static void pressPastTheGraceWaitsForTheNextLine() {
    SyncRig r; r.lp.setSync(true); r.toPhase(7680);                      // 160 ms after: past 150 ms
    r.lp.pressRec(0); CHECK(r.lp.waiting());
}
static void firstLoopIsWholeBars() {
    SyncRig r; r.lp.setSync(true); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 2 * 96000 + 1280)); r.lp.pressRec(0);   // 26.7 ms late on bar 3
    CHECK(r.lp.loopLength() == 2 * 96000 && r.lp.bars() == 2);
    SyncRig q; q.lp.setSync(true); q.toPhase(0);
    q.lp.pressRec(0); q.feedSync(sine(220, 0.1f, 2 * 96000 - 1280)); q.lp.pressRec(0);   // early: waits
    CHECK(q.lp.waiting()); q.silenceSync(1280 + 128); CHECK(q.lp.loopLength() == 2 * 96000 && q.lp.bars() == 2);
}
static void tempoChangeDuringTheFirstRecording() {                       // Review Focus 2: bars at the close tempo
    SyncRig r; r.lp.setSync(true); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 96000));
    r.spb = 80000.0; r.phase = 0.0;                                      // the tempo changes on a line
    r.feedSync(sine(220, 0.1f, 80000)); r.lp.pressRec(0);
    CHECK(r.lp.bars() == 2 && r.lp.loopLength() == 160000);              // round(176000 / 80000) = 2 bars of 80000
}
static void cancelAWaitingPress() {
    SyncRig r; r.lp.setSync(true); r.toPhase(48000);
    r.lp.pressRec(0); CHECK(r.lp.waiting()); r.lp.pressRec(0); CHECK(!r.lp.waiting());
    r.lp.pressRec(0); CHECK(r.lp.waiting()); r.lp.undo(0); CHECK(!r.lp.waiting());
    r.silenceSync(96000); CHECK(r.lp.state(0) == TrackState::Empty);
}
static void noGridActsAtOnce() {                                         // Review Focus 5
    SyncRig r; r.lp.setSync(true); r.valid = false; r.toPhase(48000);
    r.lp.pressRec(0); CHECK(!r.lp.waiting() && r.lp.state(0) == TrackState::Recording);
}
static void syncOffIsUnchanged() {                                       // identical output with and without a grid
    auto run = [](bool grid) {
        SyncRig r; r.lp.setSync(false); r.valid = grid; r.toPhase(30000);
        r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 50000)); r.lp.pressRec(0);
        r.feedSync(sine(330, 0.1f, 20000)); r.lp.pressRec(0); r.feedSync(sine(330, 0.1f, 9000)); r.lp.pressRec(0);
        r.silenceSync(60000); return r.out;
    };
    CHECK(maxAbsDiff(run(true), run(false)) == 0.0f);
}
static void freeGridStartsAtTheFirstPress() {                            // MOD stopped, no loop: the press is bar 1
    SyncRig r; r.lp.setSync(true); r.playing = false; r.phase = 12345.0;
    r.lp.pressRec(0); CHECK(!r.lp.waiting() && r.lp.state(0) == TrackState::Recording);
    r.feedSync(sine(220, 0.1f, 96000 + 640)); r.lp.pressRec(0);         // 13 ms after the free grid's bar 2
    CHECK(r.lp.loopLength() == 96000 && r.lp.bars() == 1);
}

int main() {
    earlyPressWaitsForTheLine(); latePressIsBackDatedToTheLine(); pressPastTheGraceWaitsForTheNextLine();
    firstLoopIsWholeBars(); tempoChangeDuringTheFirstRecording(); cancelAWaitingPress(); noGridActsAtOnce();
    syncOffIsUnchanged(); freeGridStartsAtTheFirstPress();
    REPORT();
}
```

Note: with `Rig`'s trigger latency 0, a plain press back-dates by 0. The tests rely on the grid alone.

- [ ] **Step 2: Run `make test`.** Expected: `test_sync.cpp` fails to compile (`setSync` is not a member).
- [ ] **Step 3: Implement the grid, the quantized first-recording start and close, cancel, the `closeFirst` back-date parameter, `bars_`, and the larger history.**
- [ ] **Step 4: Run `make test`.** Expected: `==> all tests passed`, with every existing file unchanged.
- [ ] **Step 5: Commit.** `git add -A plugins/Ouroboros/dsp tests/test_sync.cpp && git commit -m "engine: bar grid; the first recording starts and closes on bar lines"`

---

### Task 2: Engine — recordings onto the loop, auto-record and restarts on the grid

**Files:**
- Modify: `plugins/Ouroboros/dsp/Looper.hpp`
- Test: `tests/test_sync.cpp`

**Interfaces:**
- Consumes: Task 1's grid, pending action, `waiting()`, `waitingTrack()` and `bars()`.

**Behaviour to implement:**
- **A recording onto the loop starts on a line:** `pressRec(t)` on an empty track with L set uses the same grace/wait rule as Task 1, with `startDub(tr, since, …)` as the back-date. Its close stays immediate (the longer-tracks rule).
- **Auto-record with a grid:**
  - A threshold crossing within the grace window after a line starts at that line. The back-date is the samples since the line plus the pre-roll, so the pre-roll sits just before the line, as a lead-in.
  - An earlier crossing waits for the next line, then starts with back-date = the pre-roll.
- **A restart from stopped while MOD plays** (B or the Stop/Play port) waits for the next line. While MOD is stopped, it restarts at once.
- **While MOD is stopped and a loop exists,** the grid is the loop's own bars. A loop recorded with Sync Off has `bars_ = 1`, so its line is its top.

- [ ] **Step 1: Write the failing tests:**

```cpp
static void recordingOntoTheLoopStartsOnALine() {
    SyncRig r; r.lp.setSync(true); r.lp.setTailMs(0); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(std::vector<float>(96000, 0.0f)); r.lp.pressRec(0);   // a silent 1-bar loop
    r.toPhase(96000 - 1280); r.lp.pressRec(1); CHECK(r.lp.waiting() && r.lp.waitingTrack() == 1);
    r.silenceSync(1280 + 128); CHECK(r.lp.state(1) == TrackState::Recording && r.lp.trackPosition(1) <= 128);
    r.feedSync(sine(220, 0.1f, 50000)); r.lp.pressRec(1);                // the close stays immediate
    CHECK(r.lp.state(1) == TrackState::Playing && r.lp.trackLength(1) == 96000);
}
static void autoRecordStartsOnALine() {
    SyncRig r; r.lp.setSync(true); r.lp.setTailMs(0); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(std::vector<float>(96000, 0.0f)); r.lp.pressRec(0);
    r.lp.setAutoRec(true); r.lp.pressRec(1); r.toPhase(1280);
    r.feedSync(sine(220, 0.2f, 1000));                                   // crosses ~27 ms after the line
    CHECK(r.lp.state(1) == TrackState::Recording && r.lp.trackPosition(1) >= 1280 && r.lp.trackPosition(1) < 1280 + 1000 + 128);
}
static void restartWaitsForTheLineWhileModPlays() {
    SyncRig r; r.lp.setSync(true); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 96000)); r.lp.pressRec(0); r.silenceSync(20000);
    r.lp.stop(ouro::StopMode::Instant); r.silenceSync(2000); CHECK(r.lp.transport() == ouro::Transport::Stopped);
    r.toPhase(48000); r.lp.pressRec(0);
    CHECK(r.lp.waiting() && r.lp.transport() == ouro::Transport::Stopped);
    r.silenceSync(48000 + 128); CHECK(r.lp.transport() == ouro::Transport::Running);
    r.playing = false; r.lp.stop(ouro::StopMode::Instant); r.silenceSync(2000);
    r.lp.pressRec(0); CHECK(!r.lp.waiting() && r.lp.transport() == ouro::Transport::Running);   // MOD stopped: at once
}
static void syncTurnedOnOverAFreeLoop() {                                // Review Focus 1
    SyncRig r; r.lp.setTailMs(0); r.playing = false;
    r.lp.pressRec(0); r.feedSync(std::vector<float>(30000, 0.0f)); r.lp.pressRec(0);   // free, 30000 samples
    r.lp.setSync(true); r.silenceSync(1280);
    CHECK(r.lp.bars() == 1 && r.lp.loopLength() == 30000);              // no jump, one bar
    r.silenceSync((30000 - r.lp.position()) % 30000 + 10000);           // 208 ms into the loop: past the grace
    r.lp.pressRec(1); CHECK(r.lp.waiting());
    r.silenceSync(30000 - 10000 + 128); CHECK(r.lp.state(1) == TrackState::Recording);   // at the loop top
}
```

  Add them to `main`.

- [ ] **Step 2: Run `make test`.** Expected: the new checks fail.
- [ ] **Step 3: Implement the rules above.**
- [ ] **Step 4: Run `make test`.** Expected: `==> all tests passed`.
- [ ] **Step 5: Commit.** `git commit -am "engine: recordings onto the loop, auto-record and restarts on the bar grid"`

---

### Task 3: Engine — following MOD's play/stop

**Files:**
- Modify: `plugins/Ouroboros/dsp/Looper.hpp`
- Test: `tests/test_sync.cpp`

**Interfaces:**
- Produces:
  - `void Looper::modStarted(bool atLine);` The plugin calls it on a seen play edge, before `process()`.
    - `atLine` true means MOD started on a bar line (`toNextBar` ≈ 0): the retrigger happens at sample 0 of this block.
    - `atLine` false means it waits for the next line.
  - `void Looper::modStopped(StopMode mode, float fadeMs);` The plugin calls it on a stop edge.
- Behaviour:
  - **`modStarted` with the looper running:**
    - close every writer as a stop does: a recording onto the loop with its rule, an overdub as a layer;
    - shift every playhead as a park does (the longest lands on its beginning; reuse park's shift and op split);
    - crossfade over `fadeIn_` (3 ms): the old positions fade out while the new ones fade in. For those 3 ms the engine reads each track at its old position too, from the buffer only. It's a new private "retrigger" step.
  - **`modStarted` with the looper stopped:** `restart()` at that sample.
  - **`modStarted` during a first recording:** ignored.
  - **`modStarted` when every track is empty:** does nothing.
  - **`modStopped`:**
    - during a first recording with at least one bar recorded: close on the last line passed (back-date = samples since it), then stop. With less than a bar, discard it.
    - otherwise: `stop(mode, fadeMs)`.

- [ ] **Step 1: Write the failing tests:**

```cpp
static void modStartRetriggersOnTheLine() {
    SyncRig r; r.lp.setSync(true); r.lp.setTailMs(0); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(240, 0.1f, 96000)); r.lp.pressRec(0);   // 240 Hz: whole cycles in a bar
    r.silenceSync(40000); const size_t from = r.out.size() - 1;
    r.phase = 0.0; r.lp.modStarted(true); r.silenceSync(4800);
    CHECK(r.lp.position() < 4800 + 128 && r.lp.trackPosition(0) < 4800 + 128);   // back near the top
    CHECK(maxStep(slice(r.out, from, r.out.size())) < 2.5f * 2.0f * sineStep(240, 0.1f));   // no click
}
static void modStartWhileOverdubbingKeepsTheLayer() {                    // Review Focus 3
    SyncRig r; r.lp.setSync(true); r.lp.setTailMs(0); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(240, 0.1f, 96000)); r.lp.pressRec(0);
    auto base = r.loopImage();
    r.lp.pressRec(0); r.feedSync(sine(480, 0.1f, 20000));
    r.phase = 0.0; r.lp.modStarted(true); r.silenceSync(2000);
    CHECK(r.lp.state(0) == TrackState::Playing && r.lp.undoDepth(0) == 1);
    r.lp.undo(0); CHECK(maxAbsDiff(r.settle(), base) < 1e-5f);
}
static void modStartMidBarWaitsForTheLine() {                            // Review Focus 4
    SyncRig r; r.lp.setSync(true); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 96000)); r.lp.pressRec(0); r.silenceSync(10000);
    r.phase = 30000.0; const uint32_t p = r.lp.position();
    r.lp.modStarted(false); r.silenceSync(128);
    CHECK(r.lp.position() == (p + 128) % 96000 && r.lp.waiting());     // no jump yet
    r.silenceSync(66000 - 128 + 128); CHECK(!r.lp.waiting() && r.lp.position() <= 128);
}
static void modStopDuringTheFirstRecordingKeepsWholeBars() {
    SyncRig r; r.lp.setSync(true); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 96000 + 30000));
    r.lp.modStopped(ouro::StopMode::Instant, 3000.0f);
    CHECK(r.lp.loopLength() == 96000 && r.lp.bars() == 1);
    SyncRig q; q.lp.setSync(true); q.toPhase(0);
    q.lp.pressRec(0); q.feedSync(sine(220, 0.1f, 50000)); q.lp.modStopped(ouro::StopMode::Instant, 3000.0f);
    CHECK(q.lp.loopLength() == 0 && q.lp.state(0) == TrackState::Empty);
}
static void modStartFromStoppedRestarts() {
    SyncRig r; r.lp.setSync(true); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 96000)); r.lp.pressRec(0);
    r.lp.modStopped(ouro::StopMode::Instant, 3000.0f); r.silenceSync(2000);
    CHECK(r.lp.transport() == ouro::Transport::Stopped);
    r.phase = 0.0; r.lp.modStarted(true); r.silenceSync(128);
    CHECK(r.lp.transport() == ouro::Transport::Running && r.lp.position() <= 128);
}
```

  Add them to `main`.

- [ ] **Step 2: Run `make test`.** Expected: compile failure (`modStarted` doesn't exist).
- [ ] **Step 3: Implement `modStarted`/`modStopped` and the crossfaded retrigger.**
- [ ] **Step 4: Run `make test`.** Expected: `==> all tests passed`.
- [ ] **Step 5: Commit.** `git commit -am "engine: follow MOD's play/stop — retrigger on the line with a 3 ms crossfade"`

---

### Task 4: Plugin — time position, the sync port, play/stop edges

**Files:**
- Modify:
  - `plugins/Ouroboros/DistrhoPluginInfo.h` (`DISTRHO_PLUGIN_WANT_TIMEPOS 1`)
  - `plugins/Ouroboros/OuroborosPlugin.hpp`
  - `tests/lv2/test_lv2_hooks.cpp`: connect the new `lv2_events_in` port to an empty `LV2_Atom_Sequence`, not a float.
- Test: `tests/test_plugin.cpp`

**Interfaces:**
- Consumes: `setSync`, `setGrid`, `modStarted`, `modStopped`.
- Produces: `kParamSync`, inserted before `kParamBypass`, with symbol `sync`, name `Sync`, Enum Off/Bars, default 0, in the Settings group.
- `paramFromLv2Index`: the parameters start after the audio ports plus the one events-in port. Update the `static_assert` so it allows `WANT_TIMEPOS`, and update the `hmiPort` guard.

**Behaviour:**
- Each `run()`, before `fEngine.process()`, read `getTimePosition()`:
  - `valid = bbt.valid`, and `playing`;
  - `spb = bbt.beatsPerBar × 60 / bbt.beatsPerMinute × sampleRate`;
  - the position in the bar in beats is `(bbt.beat − 1) + bbt.tick / bbt.ticksPerBeat`;
  - `toNextBar = (beatsPerBar − beats) × 60 / bpm × sampleRate`. Treat values within one sample of `spb` or of 0 as 0, i.e. on the line.
  - Call `fEngine.setSync(on(kParamSync))` on change, and `fEngine.setGrid(valid, playing, spb, toNextBar)` every block.
- **Order:** call `setGrid` before handling the triggers (rec, stop_play, the C footswitch), so a press in this block sees this block's grid.
- **Edges**, only while Sync = Bars and `valid`:
  - keep the last `playing`;
  - initialise it from the current value on the first run and whenever Sync turns on, so neither counts as an edge;
  - off→on calls `fEngine.modStarted(toNextBar == 0)`;
  - on→off calls `fEngine.modStopped(stopMode, fadeMs)`.

- [ ] **Step 1: Write the failing tests.**
  - In `portSymbolsAreFrozen`:
    - add `"sync"` at index 30;
    - require `kParamBypass == 31` and a count of 32;
    - the group check covers index 30 as Settings (2);
    - `getParameterEnumValues(kParamSync).count == 2`, and the default is 0.
  - Add these tests, and call them from `main`:

```cpp
static double gT = 0.0;                                             // MOD's transport position, in samples
static void runMod(bool playing, int k) {                           // k blocks; the time position advances as MOD's would
    for (int i = 0; i < k; ++i) {
        const double beats = std::fmod(gT / 24000.0, 4.0);          // 120 BPM 4/4 at 48 kHz: 24000 samples a beat
        TimePosition t; t.playing = playing; t.frame = uint64_t(gT); t.bbt.valid = true; t.bbt.bar = 1 + int(gT / 96000.0);
        t.bbt.beat = 1 + int(beats); t.bbt.tick = (beats - int(beats)) * 1920.0; t.bbt.barStartTick = 0;
        t.bbt.beatsPerBar = 4; t.bbt.beatType = 4; t.bbt.ticksPerBeat = 1920; t.bbt.beatsPerMinute = 120;
        P().setTimePosition(t); runN(1);
        if (playing) gT += 128;
    }
}
static void syncPressWaitsForTheBar() {
    fresh(); gT = 50000; set(kParamSync, 1); runMod(true, 1);       // mid-bar
    set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0);
    CHECK(eng().waiting() && eng().state(0) == ouro::TrackState::Empty);
    runMod(true, 358);                                              // past the next bar line (96000)
    CHECK(!eng().waiting() && eng().state(0) == ouro::TrackState::Recording);
}
static void modPlayAndStopEdgesFollow() {
    fresh(); gT = 0; set(kParamSync, 1); runMod(true, 1);           // the first run is no edge
    set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0);           // 128 after the line: back-dated to it
    runMod(true, 748 - 1);                                          // to 96000 - 128
    set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0); runMod(true, 10);
    CHECK(eng().loopLength() == 96000);
    runMod(false, 1); runMod(false, 20); CHECK(eng().transport() == ouro::Transport::Stopped);   // stop edge
    gT = 0; runMod(true, 1); CHECK(eng().transport() == ouro::Transport::Running);               // play edge, on the line
}
static void syncSwitchedOnWhilePlayingIsNoEdge() {
    fresh(); gT = 30000; runMod(true, 1);                           // Sync Off: a free loop
    set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0); runMod(true, 100);
    set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0); runMod(true, 5);
    const uint32_t p = eng().position();
    set(kParamSync, 1); runMod(true, 1);
    CHECK(eng().position() == (p + 128) % eng().loopLength() && eng().transport() == ouro::Transport::Running);
}
```

  (`PluginExporter::setTimePosition` exists in `src/DistrhoPluginInternal.hpp`. `runMod` advances the position each block, as MOD's transport would.)

- [ ] **Step 2: Run `make test`.** Expected: compile failure (`kParamSync` is not declared).
- [ ] **Step 3: Implement the time-position read, the port and its mapping, the grid hand-off and the edges, and fix the LV2 hook test's port connection.**
- [ ] **Step 4: Run `make test` and `make test-lv2`.** Expected: `==> all tests passed`, and `test_lv2_hooks` prints `OK`.
- [ ] **Step 5: Commit.** `git commit -am "plugin: Sync setting; read MOD's bar grid and play/stop each block"`

---

### Task 5: HMI and widget while an action waits

**Files:**
- Modify:
  - `plugins/Ouroboros/hmi/HmiBridge.hpp`: `HmiView` gains `bool waiting`.
  - `plugins/Ouroboros/hmi/LoopWidget.hpp`: `WidgetView` gains `bool waiting`.
  - `plugins/Ouroboros/hmi/Font.hpp`: add `W` and `I`.
  - `plugins/Ouroboros/OuroborosPlugin.hpp`: fill both fields from `fEngine.waiting()` for the selected track or a restart.
- Test: `tests/test_hmi.cpp`, `tests/test_widget.cpp`

**Behaviour:**
- **B's LED while waiting:** it blinks fast (`LV2_HMI_LED_Blink_Fast` on and off) in the coming colour:
  - red while a recording waits to start;
  - green while the first loop waits to close and play;
  - green for a restart.
- **B's label while waiting:** it shows the coming action, as today.
- **The fingerprint** includes `waiting`.
- **The widget text** (tall layout) shows `WAIT` in place of the state while waiting.

- [ ] **Step 1: Write the failing tests:**
  - **HMI:** with `view(Empty, Rec, …)` plus `waiting = true`, the LED is red, with on and off both `LV2_HMI_LED_Blink_Fast`. With `view(Recording, Play, …)` plus `waiting`, it is green and fast. When waiting ends, the LED goes back to the existing state colour.
  - **Widget:** `showsText(v, "WAIT")` with `v.waiting = true`. Add `"WAIT"` to `everyWidgetStringHasGlyphs`.
- [ ] **Step 2: Run `make test`.** Expected: compile failure (no `waiting` field).
- [ ] **Step 3: Implement the fields, the LED, the text, the glyphs and the plugin fill.**
- [ ] **Step 4: Run `make test` and `make test-lv2`.** Expected: `==> all tests passed`, and `test_lv2_hooks` prints `OK`.
- [ ] **Step 5: Commit.** `git commit -am "hmi: B blinks fast and the screen shows WAIT while a press waits for the bar"`

---

### Task 6: Manual and README

**Files:**
- Modify: `docs/manual/ouroboros-manual.html` and `README.md`
- Regenerate: `docs/manual/ouroboros-manual.pdf` (`make manual`)

- [ ] **Step 1: Manual.** Add a short "Tempo sync" paragraph and a Settings row. Keep the manual's plain second-person voice. Cover:
  - **The setting:** Sync (Off / Bars, default Off).
  - **What snaps:** with Bars, recording starts and the first loop's end land on MOD's bar lines, so loops are whole bars. A press up to 150 ms late still counts as on the line; an earlier one waits. While it waits, B blinks fast and the screen shows WAIT. Press B again or tap C to cancel.
  - **Following MOD's transport:** MOD's play starts every track from its top on the bar line, and stop stops the loops (per Stop Mode).
  - **With MOD stopped:** your first press sets bar 1.
  - **Tempo changes:** loops keep their length, so they drift from the drums if the tempo changes after recording.
- [ ] **Step 2:** Run `make manual`, view the PDF, and check that nothing is cut off.
- [ ] **Step 3: README.** Add the bullet "Optional tempo sync: loops start on MOD's bar lines, are whole bars long, and follow MOD's play/stop."
- [ ] **Step 4:** Run `make && make test`. Expected: the build succeeds and `==> all tests passed`.
- [ ] **Step 5: Commit.** `git add -A docs/manual README.md && git commit -m "manual, README: tempo sync"`
