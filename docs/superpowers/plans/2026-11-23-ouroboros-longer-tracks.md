# Ouroboros Longer Tracks Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A track recorded onto an existing loop keeps recording past the loop end. B stops it at the press, and its length rounds to whole loops, counted from the start press. Every track stays locked to the first loop through overdub, undo, stop and restart.

**Architecture:** Each track gets its own length (a whole multiple of the first loop L) and its own playhead. All playheads advance together, so they stay locked. A recording onto the loop writes linearly into the track buffer, indexed from the start press, until B closes it. The close picks k whole loops by the quarter rule. Indices past `k·L` wrap onto the track's beginning lazily, as a one-off op. Parking for a stop shifts every playhead by the same amount, so that the longest track lands on its beginning.

**Tech Stack:** C++14 (gnu++14), the header-only engine in `plugins/Ouroboros/dsp/`, the repo's own `CHECK`/`REPORT` harness (`make test`), and DPF/LV2 for the plugin.

**Spec:** `docs/superpowers/specs/2026-11-23-ouroboros-longer-tracks-design.md`. It builds on `2026-11-20-ouroboros-looper-design.md` and `2026-11-23-ouroboros-stop-clear-autorecord-design.md`.

## Global Constraints

- `process()`, `run()` and every engine control are RT-safe: no allocation, no locks, no I/O. The close of a long recording must not loop over more than a few thousand samples. Everything longer happens lazily at the playhead.
- One-loop behaviour stays exactly as today: every existing test passes unchanged.
- B stops recording at the press. Whole loops count from the start press. Rounding:
  - a press within `kRoundDownFraction = 0.25` of a loop past k whole loops (k ≥ 1) rounds down to k;
  - otherwise it rounds up;
  - less than one loop gives one loop.
- The overshoot of a rounded-down close lands on the track's beginning at full level, where it falls in time.
- A long track is at most `floor(maxRec_ / L)` loops (5 minutes). At that many loops it closes as a press there.
- Only one track ever takes the input. Handover, stop and the maximum close with the same rule. Nothing carries on recording after a press.
- A reused buffer never plays stale audio, and a track never plays itself while it is recording onto the loop.
- **Restart and park:** the longest track lands on its beginning (lowest track number on ties), every other track keeps its alignment, and one-loop tracks land at the top.
- **End-of-loop stop:** waits for the longest track's pass.
- The 32 pending-op cap stays. Ops on a long track last one pass of that track. Mark this with a `ponytail:` ceiling.
- Commit after every task on branch `worktree-longer-tracks`, ending each message with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **Re-recording a track that last held a long loop:** no old audio leaks into the new recording, not while recording and not in its silent parts. Test in Task 2.
2. **Undoing a long track's base, then redoing it many passes later:** it comes back in the same alignment. Test in Task 3.
3. **After Rec = Rec > Dub after a rounded-down long close:** the overdub covers the full length, and undo restores the long track exactly. Test in Task 2.
4. **Trigger latency 3 ms:** the back-dated lead-in sits just before the start, and wraps to the track's end when the press is within 3 ms after a pass start. Test in Task 2.
5. **A long track over a very short first loop** (many loops): the screen drops the ticks instead of smearing them. Test in Task 4.

---

### Task 1: Per-track length and playhead (no behaviour change)

**Files:**
- Modify: `plugins/Ouroboros/dsp/Looper.hpp`
- Test: `tests/test_longtracks.cpp` (new; `make test` builds every `tests/test_*.cpp`)

**Interfaces:**
- Produces (public):
  - `uint32_t Looper::trackLength(int t) const;`
    - the track's length in samples: a whole multiple of L;
    - 0 when the track is empty with no base kept for redo, or (from Task 2) while it is still recording onto the loop.
  - `uint32_t Looper::trackPosition(int t) const;`
    - samples since the track's beginning, modulo its length;
    - the beginning is the pass start in which the track started, so for any non-empty track `trackPosition(t) % loopLength() == position()`.
- Internals are your call. Suggested: `Track::len`, a buffer playhead `Track::pos`, and the buffer index of the track's beginning. Constraints:
  - every per-track use of `L_` or `pos_` becomes the track's length or playhead: `play()`, `writeOut()`, `writeIn()`, `write()`, `store()`, `startDub()`, `endDub()`, `overlap()`, `addOp()`, the zero op, `park()`;
  - `pos_` stays as the base phase;
  - all playheads with a length advance together each sample (also while a base is undone and kept for redo).

In this task every track is still exactly L long:
- the first recording sets `len = L`;
- a recording onto the loop gets `len = L` at its start and wraps as today;
- clear and `reset()` set `len = 0`;
- a base undo keeps `len` for redo.

- [ ] **Step 1: Write the failing test** in `tests/test_longtracks.cpp`:

```cpp
#include "looper_rig.hpp"
using ouro::TrackState; using ouro::Transport; using ouro::StopMode;

static const uint32_t L = 24000;
static void toPos(Rig& r, uint32_t p) { r.silence((L + p - r.lp.position()) % L); }   // base phase p
static void silentBase(Rig& r) { r.lp.setTailMs(0); recordBase(r, L, 0.0f); }        // track 0: a silent loop

static void oneLoopTracksReportLengthAndPosition() {
    Rig r; silentBase(r); toPos(r, 6000);
    CHECK(r.lp.trackLength(0) == L && r.lp.trackPosition(0) == r.lp.position() && r.lp.trackLength(1) == 0);
    r.lp.pressRec(1); r.feed(sine(330, 0.1f, L)); r.lp.pressRec(1);   // exactly one loop, from mid-pass
    CHECK(r.lp.trackLength(1) == L);
    for (int i = 0; i < 5; ++i) {
        r.silence(7777);
        CHECK(r.lp.trackPosition(1) == r.lp.position() && r.lp.trackPosition(0) == r.lp.position());
    }
    r.lp.undo(1); r.silence(3000);                                    // the base (a recording onto the loop): kept for redo
    CHECK(r.lp.state(1) == TrackState::Empty && r.lp.trackLength(1) == L && r.lp.trackPosition(1) == r.lp.position());
    r.lp.clearTrack(1); r.silence(1000); CHECK(r.lp.trackLength(1) == 0);
}

int main() {
    oneLoopTracksReportLengthAndPosition();
    REPORT();
}
```

- [ ] **Step 2: Run `make test`.** Expected: `test_longtracks.cpp` fails to compile (`trackLength` is not a member).
- [ ] **Step 3: Implement the per-track length and playhead as described,** with no behaviour change.
- [ ] **Step 4: Run `make test`.** Expected: `==> all tests passed`. Every existing file passes unchanged.
- [ ] **Step 5: Commit.** `git add -A plugins/Ouroboros/dsp tests/test_longtracks.cpp && git commit -m "engine: per-track length and playhead (every track still one loop)"`

---

### Task 2: Recording past the loop end, and the close

**Files:**
- Modify: `plugins/Ouroboros/dsp/Constants.hpp` (add `constexpr float kRoundDownFraction = 0.25f;`), `plugins/Ouroboros/dsp/Looper.hpp`
- Test: `tests/test_longtracks.cpp`

**Interfaces:**
- Consumes: Task 1's `trackLength`, `trackPosition`, per-track length and playhead.
- Changes:
  - a recording onto the loop no longer wraps at L;
  - `trackLength(t)` is 0 while it records;
  - `trackPosition(t)` while it records = the base phase at the start press + samples since the press.

**Behaviour to implement:**
- **Start** (`startDub(…, base = true …)`, from a press, a handover or an arm):
  - the buffer index is the number of samples since the start press;
  - the writer writes at (index − back), where back = trigger latency + offset, as today;
  - the back-dated part before index 0 (at most back samples) is kept aside in bounded storage set up in `init()`, for example the track buffer's 1 s slack. Clear the aside area first;
  - the track's beginning is the pass start containing the press: base phase p at the press means the beginning is p samples before index 0.
- **While recording:**
  - the playhead zeroes the buffer at its own position before reading it, so the track plays silence (no stale audio, no hearing itself);
  - writes are linear;
  - the existing one-pass zero op is not needed for this.
- **Close** (`close(u, …)` for a recording onto the loop: B, handover, stop):
  - n = samples since the start press (the playhead index);
  - k = `n < L ? 1 : (n % L <= kRoundDownFraction * L ? n / L : n / L + 1)`;
  - `len = k·L`; the playhead becomes `n % len` for a rounded-down close, else n;
  - from now on the writer (and its 5 ms punch-out fade) wraps at `len`;
  - **rounded down** (n ≥ len): the samples already written at buffer indices [len, w) are added onto [0, w − len) by a one-off op. It applies as the playhead reaches each index on the next pass and reads its source at index + len, like the zero op. No bulk copy;
  - **rounded up:** the unrecorded rest of the buffer reads as zero via a zero op from the playhead to the end;
  - the lead-in kept aside is added onto [len − b, len). Zero that range first wherever it was not recorded;
  - the 5 ms punch-out fade happens as today.
- **The maximum:** when n reaches `(maxRec_ / L)·L` while recording onto the loop, close as a press there. Exact multiple: k = maxRec_ / L, no overshoot.
- **After Rec = Rec > Dub:** after the close, the overdub starts as today, over the full length.
- **Auto-record:** an arm onto the loop starts exactly like a press.

The one-off add op ("fold") reuses `Op` with a new flag and reads its source from the track buffer at `at + len`. It splits at a park like any op. A clear or wipe drops it.

- [ ] **Step 1: Write the failing tests:**

```cpp
// Track t alone, over its whole length, indexed by trackPosition (0 = the track's beginning).
static std::vector<float> trackImage(Rig& r, int t) {
    for (int u = 0; u < ouro::kTracks; ++u) r.lp.setOn(u, u == t);
    r.silence(1000);
    const uint32_t len = r.lp.trackLength(t), p0 = r.lp.trackPosition(t);
    auto o = r.capture(len);
    std::vector<float> img(len);
    for (uint32_t i = 0; i < len; ++i) img[(p0 + i) % len] = o[i];
    for (int u = 0; u < ouro::kTracks; ++u) r.lp.setOn(u, true);
    return img;
}
// Track 1 from base phase 6000: d samples of a 220 Hz 0.1 sine, B, then afterN samples of `after`.
static std::vector<float> recordLong(Rig& r, uint32_t d, float after = 0.0f, uint32_t afterN = 0) {
    toPos(r, 6000);
    auto x = sine(220, 0.1f, d + afterN);
    for (uint32_t j = d; j < d + afterN; ++j) x[j] = after;
    r.lp.pressRec(1); r.feed(slice(x, 0, d)); r.lp.pressRec(1); r.feed(slice(x, d, d + afterN));
    return x;
}
static float errAt(const std::vector<float>& img, uint32_t base, const std::vector<float>& x, uint32_t j0, uint32_t j1) {
    float e = 0.0f; for (uint32_t j = j0; j < j1; ++j) e = std::max(e, std::fabs(img[(base + j) % img.size()] - x[j]));
    return e;
}
static float peakIn(const std::vector<float>& img, uint32_t a, uint32_t b) {   // [a, b) mod size
    float m = 0.0f; for (uint32_t i = a; i < b; ++i) m = std::max(m, std::fabs(img[i % img.size()]));
    return m;
}

static void longRecordingRoundsUp() {                                  // 1.6 loops -> 2, the rest silent
    Rig r; silentBase(r); auto x = recordLong(r, 38400); r.silence(1000);
    CHECK(r.lp.trackLength(1) == 2 * L && r.lp.state(1) == TrackState::Playing);
    auto img = trackImage(r, 1);
    CHECK(errAt(img, 6000, x, 200, 38400 - 1) < 1e-6f);
    CHECK(peakIn(img, 6000 + 38400 + 300, 2 * L + 6000) == 0.0f);      // to the end, then [0, 6000)
}
static void longRecordingRoundsDownOntoTheStart() {                    // 2.2 loops -> 2, overshoot on top
    Rig r; silentBase(r); auto x = recordLong(r, 52800); r.silence(1000);
    CHECK(r.lp.trackLength(1) == 2 * L);
    auto img = trackImage(r, 1);
    float e = 0.0f;
    for (uint32_t j = 200; j < 4700; ++j) e = std::max(e, std::fabs(img[6000 + j] - (x[j] + x[48000 + j])));
    CHECK(e < 1e-6f && errAt(img, 6000, x, 5100, 47900) < 1e-6f);
}
static void oneLoopFromMidPassStaysOneLoop() {
    Rig r; silentBase(r); auto x = recordLong(r, L); r.silence(1000);
    CHECK(r.lp.trackLength(1) == L && errAt(trackImage(r, 1), 6000, x, 200, L - 300) < 1e-6f);
}
static void lessThanALoopIsOneLoop() {
    Rig r; silentBase(r); auto x = recordLong(r, 12000); r.silence(1000);
    auto img = trackImage(r, 1);
    CHECK(r.lp.trackLength(1) == L && errAt(img, 6000, x, 200, 11900) < 1e-6f && peakIn(img, 18300, L + 6000) == 0.0f);
}
static void roundingCountsFromTheStartPress() {
    { Rig r; silentBase(r); recordLong(r, 26400); CHECK(r.lp.trackLength(1) == L); }       // 1.1 loops
    { Rig r; silentBase(r); recordLong(r, 31200); CHECK(r.lp.trackLength(1) == 2 * L); }   // 1.3 loops
    { Rig r; silentBase(r); recordLong(r, 3 * L + 5000); CHECK(r.lp.trackLength(1) == 3 * L); }
}
static void bStopsAtThePress() {                                       // loud playing after B stays out
    Rig r; silentBase(r); recordLong(r, 38400, 0.3f, 12000); r.silence(1000);
    CHECK(peakIn(trackImage(r, 1), 6000 + 38400 + 300, 2 * L + 6000) == 0.0f);
}
static void handoverAndStopCloseWithTheRule() {
    { Rig r; silentBase(r); toPos(r, 6000); r.lp.pressRec(1); r.feed(sine(220, 0.1f, 38400)); r.lp.pressRec(2);
      CHECK(r.lp.state(1) == TrackState::Playing && r.lp.trackLength(1) == 2 * L && r.lp.state(2) == TrackState::Recording); }
    { Rig r; silentBase(r); toPos(r, 6000); r.lp.pressRec(1); r.feed(sine(220, 0.1f, 52800)); r.lp.stop(StopMode::Instant);
      CHECK(r.lp.trackLength(1) == 2 * L); }
}
static void maximumClosesAtWholeLoops() {                              // 8 kHz: maxRec 2.4M, L 4000 -> 600 loops
    Rig r(8000.0); r.lp.setTailMs(0); recordBase(r, 4000, 0.0f);
    r.lp.pressRec(1); r.feed(std::vector<float>(2400000 + 8000, 0.05f));
    CHECK(r.lp.state(1) == TrackState::Playing && r.lp.trackLength(1) == 2400000u);
}
static void reusedBufferNeverPlaysOldAudio() {                         // Review Focus 1
    Rig r; silentBase(r);
    toPos(r, 6000); r.lp.pressRec(1); r.feed(sine(220, 0.3f, 2 * L)); r.lp.pressRec(1); r.silence(1000);
    r.lp.clearTrack(1); r.silence(2000);
    r.lp.setOn(0, false);
    toPos(r, 6000); r.lp.pressRec(1);
    auto during = r.capture(36000);                                    // recording silence, 1.5 loops
    r.lp.pressRec(1); r.silence(1000);
    CHECK(peak(during) == 0.0f && r.lp.trackLength(1) == 2 * L && peak(trackImage(r, 1)) == 0.0f);
}
static void leadInSitsBeforeTheStartOrWrapsToTheEnd() {                // Review Focus 4: D = 144
    for (uint32_t press : {6000u, 50u}) {
        Rig r; silentBase(r); r.lp.setTriggerLatencyMs(3.0f);
        toPos(r, (press + L - 144) % L); r.feed(std::vector<float>(144, 0.2f));   // the lead-in, then the press
        r.lp.pressRec(1); r.silence(38400); r.lp.pressRec(1); r.silence(1000);
        auto img = trackImage(r, 1);
        float e = 0.0f;
        for (uint32_t i = 0; i < 144; ++i)
            e = std::max(e, std::fabs(img[(press + 2 * L - 144 + i) % (2 * L)] - 0.2f * fadeInGain(i)));
        CHECK(r.lp.trackLength(1) == 2 * L && e < 1e-6f);
    }
}
static void autoRecordCanMakeALongTrack() {
    Rig r; silentBase(r); r.lp.setAutoRec(true); r.lp.pressRec(1); r.silence(2000);
    r.feed(sine(220, 0.1f, 52800)); r.lp.pressRec(1);
    CHECK(r.lp.trackLength(1) == 2 * L);
}
static void recToDubAfterALongCloseUndoesExactly() {                   // Review Focus 3
    auto run = [](bool recToDub) {                                     // same input in both: the 550 Hz after B
        Rig r; silentBase(r); r.lp.setRecToDub(recToDub); recordLong(r, 52800);
        if (recToDub) CHECK(r.lp.state(1) == TrackState::Overdubbing);
        r.feed(sine(550, 0.1f, 2 * L));                                // only the punch-out fade keeps any of it alone
        if (recToDub) { r.lp.pressRec(1); CHECK(r.lp.undoDepth(1) == 1); r.lp.undo(1); }
        r.silence(2 * L + 3000);
        return trackImage(r, 1);
    };
    CHECK(maxAbsDiff(run(true), run(false)) < 1e-5f);
}
static void longTrackSeamIsContinuous() {                              // the overshoot continues the input
    Rig r; silentBase(r); recordLong(r, 52800); r.silence(1000);
    CHECK(maxStep(wrap(trackImage(r, 1))) < 2.5f * 2.0f * sineStep(220, 0.1f));
}
```

  Add all of these to `main`.

- [ ] **Step 2: Run `make test`.** Expected: `test_longtracks.cpp` fails to compile (`kRoundDownFraction` unused is fine), or prints FAIL lines from every new test.
- [ ] **Step 3: Implement the linear recording, the close rule, the fold op, the lead-in placement, the stale-audio zeroing and the maximum,** as described above.
- [ ] **Step 4: Run `make test`.** Expected: `==> all tests passed`, with every existing file unchanged.
- [ ] **Step 5: Commit.** `git commit -am "engine: record past the loop end; the length rounds to whole loops from the start press"`

---

### Task 3: Overdub, undo, stop and restart on long tracks

**Files:**
- Modify: `plugins/Ouroboros/dsp/Looper.hpp`
- Test: `tests/test_longtracks.cpp`

**Interfaces:**
- Consumes: Tasks 1–2.
- Changes:
  - **`park()`:**
    - pick the longest non-empty track m (lowest index on ties);
    - shift = samples from m's playhead to m's beginning;
    - every track with a length moves its playhead by shift (mod its own length), and `pos_` by shift (mod L);
    - each op's owed stretch is split at its own track's new playhead, generalising the round-1 split: rotate by the new playhead, then apply the same rule.
  - **End of loop:** `stop(StopMode::EndOfLoop)` waits for m's pass to end (m reaches its beginning) instead of L's.
  - Add a `ponytail:` comment at `kMaxPendingOps` or `addOp`: ops on a long track last one pass of that track.

- [ ] **Step 1: Write the failing tests:**

```cpp
// Track t recorded from base phase `phase`, exactly k loops of a sine (frequency hz, 0.1).
static void recordLoops(Rig& r, int t, uint32_t phase, uint32_t k, double hz) {
    toPos(r, phase); r.lp.pressRec(t); r.feed(sine(hz, 0.1f, k * L)); r.lp.pressRec(t); r.silence(500);
}
static void overdubUndoRedoOnALongTrack() {
    Rig r; silentBase(r); recordLoops(r, 1, 6000, 2, 220); auto img0 = trackImage(r, 1);
    r.lp.pressRec(1); r.feed(sine(550, 0.1f, 60000)); r.lp.pressRec(1); r.silence(2 * L);
    auto img1 = trackImage(r, 1);
    CHECK(r.lp.undoDepth(1) == 1 && maxAbsDiff(img0, img1) > 0.05f);
    r.lp.undo(1); r.silence(2 * L + 1000); CHECK(maxAbsDiff(trackImage(r, 1), img0) < 1e-5f);
    r.lp.redo(1); r.silence(2 * L + 1000); CHECK(maxAbsDiff(trackImage(r, 1), img1) < 1e-5f);
}
static void pendingUndoOnALongTrackSurvivesStop() {
    Rig r; silentBase(r); recordLoops(r, 1, 6000, 3, 220); auto img0 = trackImage(r, 1);
    r.lp.pressRec(1); r.feed(sine(550, 0.1f, 30000)); r.lp.pressRec(1); r.silence(3 * L);
    r.lp.undo(1); r.silence(20000);                                    // a fraction of the 3-loop pass applied
    r.lp.stop(StopMode::Instant); r.silence(1000); CHECK(r.lp.transport() == Transport::Stopped);
    r.lp.restart(); r.silence(3 * L + 1000);
    CHECK(maxAbsDiff(trackImage(r, 1), img0) < 1e-5f && r.lp.pendingOps(1) == 0);
}
static void parkPutsTheLongestOnItsBeginning() {
    Rig r; silentBase(r); recordLoops(r, 1, 6000, 2, 220); recordLoops(r, 2, 15000, 3, 330);
    r.silence(41234); r.lp.stop(StopMode::Instant); r.silence(1000);
    CHECK(r.lp.transport() == Transport::Stopped && r.lp.trackPosition(2) == 0 && r.lp.position() == 0
          && r.lp.trackPosition(0) == 0 && r.lp.trackPosition(1) % L == 0);
}
static void endOfLoopWaitsForTheLongest() {
    Rig r; silentBase(r); recordLoops(r, 1, 6000, 3, 220); r.silence(10000);
    const uint32_t rest = 3 * L - r.lp.trackPosition(1);
    r.lp.stop(StopMode::EndOfLoop);
    r.silence(rest - 1); CHECK(r.lp.transport() == Transport::Stopping);
    r.silence(1); CHECK(r.lp.transport() == Transport::Stopped && r.lp.trackPosition(1) == 0 && r.lp.position() == 0);
}
static void twoLongTracksKeepTheirAlignment() {                        // the arrangement is the same after a restart
    Rig r; silentBase(r); recordLoops(r, 1, 6000, 2, 220); recordLoops(r, 2, 15000, 3, 330);
    for (int u = 0; u < ouro::kTracks; ++u) r.lp.setOn(u, true);
    r.silence(1000);
    auto pre = r.capture(12 * L);                                      // two joint cycles (6 loops each)
    r.silence(33333); r.lp.stop(StopMode::Instant); r.silence(1000); r.lp.restart();
    auto post = r.capture(3 * L);
    bool found = false;
    for (uint32_t o = 0; o < 6 * L && !found; ++o) {
        float e = 0.0f;
        for (uint32_t i = 200; i < 3 * L && e < 1e-5f; ++i) e = std::max(e, std::fabs(pre[o + i] - post[i]));
        found = e < 1e-5f;
    }
    CHECK(found);
}
static void baseUndoRedoOfALongTrackKeepsAlignment() {                 // Review Focus 2
    Rig r; silentBase(r); recordLoops(r, 1, 6000, 2, 220);
    const uint32_t before = r.lp.trackPosition(1);
    r.lp.undo(1); CHECK(r.lp.state(1) == TrackState::Empty && r.lp.canRedo(1));
    r.silence(5 * L + 777);
    r.lp.redo(1);
    CHECK(r.lp.state(1) == TrackState::Playing && r.lp.trackPosition(1) == (before + 5 * L + 777) % (2 * L));
}
```

  Add them to `main`.

- [ ] **Step 2: Run `make test`.** Expected: `test_longtracks` prints FAIL lines, at least from park, End-of-loop and alignment.
- [ ] **Step 3: Implement the park shift, the generalised op split, the End-of-loop wait and the ponytail note.**
- [ ] **Step 4: Run `make test`.** Expected: `==> all tests passed`.
- [ ] **Step 5: Commit.** `git commit -am "engine: stop and restart with long tracks — the longest lands on its beginning"`

---

### Task 4: The progress bar for long tracks

**Files:**
- Modify: `plugins/Ouroboros/hmi/LoopWidget.hpp`, `plugins/Ouroboros/OuroborosPlugin.hpp` (`drawWidgets()`)
- Test: `tests/test_widget.cpp`

**Interfaces:**
- Consumes: `Looper::trackLength(int)`, `Looper::trackPosition(int)`.
- Produces: `WidgetView` gains `uint32_t trackLength, trackPosition;` for the selected track. Zero-initialised, the bar behaves as today. `drawWidgets()` fills them from the engine.

**Behaviour** (40×37: bar rows 27..33, inner rows 28..32, inner columns 1..38):
- **trackLength > 0:** the fill is `trackPosition / trackLength` across the 38 inner columns, as the position fill is today.
- **Boundary ticks:** with k = trackLength / loopLength > 1 and `38 / k >= 3`, there is a tick at each column `1 + round(38 * i / k)` for i = 1..k−1. A tick inverts the bar's top inner row pixel at that column: lit in the empty part, dark in the filled part.
- **trackLength == 0, loopLength > 0, and the selected track Recording** (a recording onto the loop):
  - the fill shows `position / loopLength` as today;
  - completed passes `trackPosition / loopLength` show as lit pixels in the bottom inner row at columns 2, 4, 6, … (at most 18).
- **The first recording** (loopLength == 0) is unchanged: elapsed time.

- [ ] **Step 1: Write the failing tests** in `tests/test_widget.cpp` and add them to `main`:

```cpp
static void longTrackBarHasPassTicks()
{
    auto v = view(); v.state[0] = TrackState::Playing; v.loopLength = 1000; v.trackLength = 3000; uint8_t b[1024];
    v.trackPosition = 0; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(px(b, 40, 14, 28) == 1 && px(b, 40, 26, 28) == 1 && px(b, 40, 13, 28) == 0 && px(b, 40, 15, 28) == 0);
    v.trackPosition = 1500; ouro::renderLoopWidget(v, 40, 37, b);   // half: filled to column 19
    CHECK(px(b, 40, 14, 28) == 0 && px(b, 40, 13, 28) == 1 && px(b, 40, 26, 28) == 1 && px(b, 40, 25, 28) == 0);
    int lit = 0; for (int x = 1; x < 39; ++x) lit += px(b, 40, x, 30);
    CHECK(std::abs(lit - 19) <= 1);
}
static void crowdedTicksAreLeftOut()                                   // Review Focus 5: 20 loops, 1.9 px apart
{
    auto v = view(); v.state[0] = TrackState::Playing; v.loopLength = 100; v.trackLength = 2000; v.trackPosition = 0;
    uint8_t b[1024]; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(litIn(b, 40, 1, 28, 39, 29) == 0);
}
static void recordingShowsPassesSoFar()
{
    auto v = view(); v.state[0] = TrackState::Recording; v.loopLength = 1000; v.position = 0; v.trackPosition = 2500;
    uint8_t b[1024]; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(px(b, 40, 2, 32) == 1 && px(b, 40, 4, 32) == 1 && px(b, 40, 6, 32) == 0);
}
```

- [ ] **Step 2: Run `make test`.** Expected: `test_widget.cpp` fails to compile (no `trackLength` member).
- [ ] **Step 3: Implement the fields, the fill, the ticks and the pass dots, and fill the fields in `drawWidgets()`.**
- [ ] **Step 4: Run `make test` and `make test-lv2`.** Expected: `==> all tests passed`, and `test_lv2_hooks` prints `OK`.
- [ ] **Step 5: Commit.** `git commit -am "widget: the bar follows a long track, with ticks at loop boundaries"`

---

### Task 5: Manual and README

**Files:**
- Modify: `docs/manual/ouroboros-manual.html`, `README.md`
- Regenerate: `docs/manual/ouroboros-manual.pdf` (`make manual`)

- [ ] **Step 1: Manual.** Add a short "Long tracks" paragraph after the Footswitch B table, in the manual's plain second-person voice:
  - Record a track over an existing loop and keep playing past the loop end, and it becomes 2, 3 or more loops long.
  - B stops recording when you press it. The length rounds to whole loops, counted from when you started:
    - pressed within a quarter loop after a whole number of loops, the bit you played past it lands on the track's start;
    - pressed later, the rest of the last loop stays silent.
  - It stays locked to the first loop.
  - Restart begins the longest track from its start.
  - End of loop waits for the longest track.
  - The bar on the screen follows the selected track, with a tick at each loop.
- [ ] **Step 2:** Run `make manual`, view the PDF, and check that nothing is cut off and the page count is the same or +1.
- [ ] **Step 3: README.** Add the bullet "Tracks can be longer than the first loop: keep playing past the end." after the per-track bullet.
- [ ] **Step 4:** Run `make && make test`. Expected: the build succeeds and `==> all tests passed`.
- [ ] **Step 5: Commit.** `git add -A docs/manual README.md && git commit -m "manual, README: long tracks"`
