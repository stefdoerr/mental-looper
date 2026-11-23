# Ouroboros Round 1 (stop/restart, clear all, auto-record) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Double-tap C stops every track; B restarts them from the top; holding C while stopped clears everything; optional auto-record arms an empty track and starts on the first note.

**Architecture:** The transport (running / stopping / stopped), the restart-safe pending ops and the auto-record arm all live in the engine (`dsp/Looper.hpp`). The engine is the only part that knows the playhead, the writers and the pending changes. The plugin adds the ports and the C footswitch gestures. The HMI bridge and the widget show the new states. The engine stays header-only and RT-safe: everything is allocated in `init()`.

**Tech Stack:** C++14 (gnu++14), DPF (LV2), the repo's own `CHECK`/`REPORT` test harness (`make test`), MOD HMI and aLabs widget extensions.

**Spec:** `docs/superpowers/specs/2026-11-23-ouroboros-stop-clear-autorecord-design.md`. It builds on `docs/superpowers/specs/2026-11-20-ouroboros-looper-design.md`.

## Global Constraints

- `process()`, `run()` and every engine control are RT-safe: no allocation, no locks, no I/O.
- Existing port symbols and their order stay unchanged. New ports go after `trigger_latency`, and bypass stays last.
- Stop mode values: Instant (default), End of loop, Fade. Fade time defaults to 3 s.
- Auto Rec is off by default. Threshold: −60…−10 dBFS, default −40. Pre-roll: a fixed 10 ms.
- The double-tap window is a setting, default 300 ms. Clear all needs a hold of 2 × Hold Time.
- Nothing may click. Restart fades in over the 3 ms recording fade-in (`kFadeInMs`). The Instant stop and a cancelled stop use the 15 ms ramp (`kRampMs`).
- Stopping never changes the loop: a tail or punch-out fade still running finishes before the engine parks.
- `make test` stays green after every task. Commit after every task, on branch `looper-v1`.

## Review Focus

1. **Undo while stopped, then restart.** The change is applied once, from the top. It is neither lost nor applied twice. Test in Task 1.
2. **Stop and restart twice within one pass while an undo is still being applied.** The buffer ends up equal to a run without stops. Test in Task 1.
3. **Every track cleared while stopped.** L resets, the transport is Running again, and the next B records. Test in Task 1.
4. **Double-tap during a first recording shorter than the minimum loop.** The recording is discarded, there is nothing to stop, the transport stays Running, and no late undo fires. Test in Task 4.
5. **Tap C, then change track before the double-tap window runs out.** The undo lands on the track that was tapped. Test in Task 4.

---

### Task 1: Engine transport — Instant stop, park, restart, restart-safe ops

**Files:**
- Modify: `plugins/Ouroboros/dsp/Looper.hpp`
- Test: `tests/test_transport.cpp` (new; `make test` picks up `tests/test_*.cpp` by itself)

**Interfaces:**
- Produces (public, in `namespace ouro`):
  - `enum class Transport : uint8_t { Running, Stopping, Stopped };`
  - `enum class StopMode : uint8_t { Instant, EndOfLoop, Fade };` In this task EndOfLoop and Fade behave as Instant. Task 2 implements them.
  - `void Looper::stop(StopMode mode, float fadeMs = 3000.0f);`
  - `void Looper::restart();`
  - `Transport Looper::transport() const;`
- Changed behaviour:
  - `pressRec(t)` while the transport is not Running calls `restart()` and returns. It never records or overdubs then.
  - `reset()` sets the transport back to Running.

**Behaviour to implement:**
- **`stop()`:**
  - Ignored when disabled or already Stopped.
  - Otherwise it closes every writing track with `close(u, false)`, as the loop in `pressRec` does.
  - If `L_ == 0` or every track is Empty afterwards, there is nothing to stop: return, still Running.
  - Else the transport becomes Stopping, and a loop gain fades from its current value to 0 over `ramp_` (15 ms).
- **The loop gain:**
  - It multiplies the sum of the tracks' `play()` outputs only. The dry path is not affected.
  - Fade curve: `g0 * (1 - t)^3` with `t = done / len`. Task 2 reuses it with a wait and other lengths, so keep it as one schedule: samples to wait, fade length, done.
- **Parking:**
  - While Stopping, the engine processes as normal: ops apply, and tails and punch-out fades keep writing input.
  - It parks after the sample on which the fade has ended, and every track's `tailEnd == 0 && fade.left == 0`.
  - Parking means: split the ops (below), set `pos_ = 0`, and set the transport to Stopped.
- **Stopped:**
  - `process()` writes the history (with a used share of 0) and outputs only the dry path.
  - No `play()`, no writes, no `pos_` advance.
- **`restart()` from Stopped:**
  - The transport becomes Running.
  - Every track's `gain` snaps to `target()`.
  - The loop gain snaps to 0 and ramps to 1 over `fadeIn_`.
  - `restart()` while Stopping is Task 2; in this task, ignore it.
- **The idle reset:** while Stopped, the reset check uses "every track Empty" instead of `idle()`, because the track gains don't advance while stopped. That is: `if (L_ && (tp_ == Transport::Stopped ? allEmpty() : idle())) reset();`
- **Restart-safe ops:**
  - `Op` gains `uint32_t at`, the next loop position it owes.
  - `addOp` and the zero op in `startDub` set `at = pos_`.
  - In `play()`, an op applies only when `pos_ == op.at`: apply, then advance `at` (wrapping at `L_`), then `++done`, and end the op when `done == len`. An op whose `at` is not the playhead is skipped and does not count `done`.
  - While running this is identical to today, because every op is created at the playhead.
  - The op array grows to `Op ops[2 * (kMaxPendingOps + 1)];` so a park can split every op. The existing caps stay as they are, so `nOps <= kMaxPendingOps + 1` whenever a park happens.

**The park split** (not determined by the tests):

```
for each track:
  for each op: op.ramp = false               // after a park the playhead only reads applied audio
  w = number of ops with op.at != 0 && op.at + (op.len - op.done) > L_
  shift ops[0, nOps) right by w; then, in the original order, for each wrapping op o:
      A = o; A.at = 0; A.len = o.at + (o.len - o.done) - L_; A.done = 0;
      A.owns = false; A.c = DeltaCursor{}; if (A.layer >= 0) ++layers[A.layer].refs;
      write A into the next free slot of the first w
      o.len = L_ - o.at; o.done = 0            // o keeps owns/layer: it ends last
  nOps += w
pos_ = 0
```

The A parts go before all other ops. That keeps true the existing rule in `dropLayer` (the last op in array order ends last), because every A part ends before every B part and before every op that wasn't split.

- [ ] **Step 1: Write the failing tests** in `tests/test_transport.cpp` (`#include "looper_rig.hpp"`, `using ouro::Transport; using ouro::StopMode;`):

```cpp
// Track 0: base 24064 (tail 0) + a 9600-sample 330 Hz layer, settled. Returns the base image.
static std::vector<float> baseAndLayer(Rig& r) {
    r.lp.setTailMs(0); auto base = recordBase(r, 24064, 0.1f);
    r.lp.pressRec(0); r.feed(sine(330, 0.1f, 9600)); r.lp.pressRec(0); r.settle();
    return base;
}
static const float kBound = 2.5f * (sineStep(220, 0.1f) + sineStep(330, 0.1f));

static void instantStopFadesOutAndParks() {
    Rig r; baseAndLayer(r); r.silence(5000); const size_t from = r.out.size() - 1;
    r.lp.stop(StopMode::Instant); CHECK(r.lp.transport() == Transport::Stopping);
    r.silence(720 + 128); CHECK(r.lp.transport() == Transport::Stopped && r.lp.position() == 0);
    r.silence(4800);
    CHECK(peak(slice(r.out, r.out.size() - 4800, r.out.size())) == 0.0f);
    CHECK(maxStep(slice(r.out, from, r.out.size())) < kBound);
}
static void restartPlaysFromTheTop() {
    Rig r; baseAndLayer(r); auto img = r.loopImage();
    r.silence(5000); r.lp.stop(StopMode::Instant); r.silence(4800);
    r.lp.restart(); CHECK(r.lp.transport() == Transport::Running);
    const size_t from = r.out.size() - 1; auto o = r.capture(4800);
    CHECK(maxAbsDiff(slice(o, 144, 4800), slice(img, 144, 4800)) < 1e-6f);   // after the 3 ms fade-in
    CHECK(maxStep(slice(r.out, from, r.out.size())) < kBound);
}
static void pendingUndoSurvivesStopAndRestart() {
    Rig r; auto base = baseAndLayer(r);
    r.silence(3000); r.lp.undo(0); r.silence(6000);              // the undo is a quarter applied
    r.lp.stop(StopMode::Instant); r.silence(1000); CHECK(r.lp.transport() == Transport::Stopped);
    r.lp.restart();
    CHECK(maxAbsDiff(r.settle(), base) < 1e-5f && r.lp.pendingOps(0) == 0);
}
static void zeroOpSurvivesStopAndRestart() {
    auto run = [](bool withStop) {
        Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.0f); recordBaseOn(r, 1, 0.2f); r.settle();
        r.lp.clearTrack(1); r.silence(2000);
        r.lp.pressRec(1); r.feed(sine(440, 0.1f, 4000)); r.lp.pressRec(1);   // onto the reused buffer
        r.silence(3000);                                                    // the zero op is still running
        if (withStop) { r.lp.stop(StopMode::Instant); r.silence(1000); r.lp.restart(); }
        auto img = r.settle(); CHECK(r.lp.pendingOps(1) == 0);
        return img;
    };
    auto a = run(true); size_t loud = 0; for (float v : a) loud += std::fabs(v) > 1e-6f;
    CHECK(maxAbsDiff(a, run(false)) < 1e-6f && loud <= 4000 + 240);
}
static void tailFinishesBeforeParking() {
    auto run = [](bool withStop) {
        Rig r; auto in = sine(220, 0.1f, 48000);                   // tail 50 ms = 2400
        r.lp.pressRec(0); r.feed(slice(in, 0, 24064)); r.lp.pressRec(0);
        if (withStop) r.lp.stop(StopMode::Instant);
        r.feed(slice(in, 24064, 24064 + 1200));
        if (withStop) CHECK(r.lp.transport() == Transport::Stopping);   // fade done, tail still writing
        r.feed(slice(in, 24064 + 1200, 24064 + 4800));
        if (withStop) { CHECK(r.lp.transport() == Transport::Stopped); r.lp.restart(); }
        return r.settle();
    };
    CHECK(maxAbsDiff(run(true), run(false)) < 1e-6f);
}
static void stopRestartTwiceWithinAPass() {                       // Review Focus 2
    Rig r; auto base = baseAndLayer(r);
    r.silence(3000); r.lp.undo(0); r.silence(6000);
    r.lp.stop(StopMode::Instant); r.silence(1000); r.lp.restart();
    r.silence(2000);                                              // split parts still owed
    r.lp.stop(StopMode::Instant); r.silence(1000); r.lp.restart();
    CHECK(maxAbsDiff(r.settle(), base) < 1e-5f && r.lp.pendingOps(0) == 0);
}
static void undoWhileStoppedAppliesFromTheTop() {                 // Review Focus 1
    Rig r; auto base = baseAndLayer(r); r.silence(5000);
    r.lp.stop(StopMode::Instant); r.silence(1000);
    r.lp.undo(0); CHECK(r.lp.undoDepth(0) == 0 && r.lp.canRedo(0));
    r.lp.restart(); CHECK(maxAbsDiff(r.settle(), base) < 1e-5f);
}
static void clearedWhileStoppedResets() {                         // Review Focus 3
    Rig r; baseAndLayer(r); recordBaseOn(r, 1); r.settle();
    r.lp.stop(StopMode::Instant); r.silence(1000);
    r.lp.clearTrack(0); r.silence(128);
    CHECK(r.lp.transport() == Transport::Stopped && r.lp.loopLength() > 0);
    r.lp.clearAll(); r.silence(128);
    CHECK(r.lp.loopLength() == 0 && r.lp.transport() == Transport::Running);
    r.lp.pressRec(0); CHECK(r.lp.state(0) == ouro::TrackState::Recording);
}
static void bWhileStoppedOnlyRestarts() {
    Rig r; baseAndLayer(r); r.lp.stop(StopMode::Instant); r.silence(1000);
    r.lp.pressRec(1);                                             // track 1 is empty: still just a restart
    CHECK(r.lp.transport() == Transport::Running && r.lp.state(1) == ouro::TrackState::Empty);
    r.lp.pressRec(1); CHECK(r.lp.state(1) == ouro::TrackState::Recording);
}
static void stopClosesWritersAndNeedsALoop() {
    { Rig r; r.lp.stop(StopMode::Instant); CHECK(r.lp.transport() == Transport::Running); }
    { Rig r; r.lp.pressRec(0); r.feed(sine(220, 0.1f, 2400)); r.lp.stop(StopMode::Instant);   // < 100 ms
      CHECK(r.lp.state(0) == ouro::TrackState::Empty && r.lp.loopLength() == 0 && r.lp.transport() == Transport::Running); }
    { Rig r; r.lp.pressRec(0); r.feed(sine(220, 0.1f, 24064)); r.lp.stop(StopMode::Instant);
      CHECK(r.lp.loopLength() == 24064 && r.lp.transport() == Transport::Stopping); }
    { Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.1f);
      r.lp.pressRec(0); r.feed(sine(330, 0.1f, 4800)); r.lp.stop(StopMode::Instant);
      CHECK(r.lp.state(0) == ouro::TrackState::Playing && r.lp.undoDepth(0) == 1); }
    { Rig r; baseAndLayer(r); r.lp.stop(StopMode::Instant); r.silence(1000); r.lp.reset();
      CHECK(r.lp.transport() == Transport::Running); }
}
int main() {
    instantStopFadesOutAndParks(); restartPlaysFromTheTop(); pendingUndoSurvivesStopAndRestart();
    zeroOpSurvivesStopAndRestart(); tailFinishesBeforeParking(); stopRestartTwiceWithinAPass();
    undoWhileStoppedAppliesFromTheTop(); clearedWhileStoppedResets(); bWhileStoppedOnlyRestarts();
    stopClosesWritersAndNeedsALoop();
    REPORT();
}
```

- [ ] **Step 2: Run it to see it fail.** Run `make test`. Expected: `tests/test_transport.cpp` fails to compile (`'Transport' is not a member of 'ouro'`).
- [ ] **Step 3: Implement the transport, parking, restart and op `at`/split in `plugins/Ouroboros/dsp/Looper.hpp`,** as described above. Keep each existing comment's meaning accurate where you touch it, for example "op applies at the playhead".
- [ ] **Step 4: Run `make test`.** Expected: every file prints `OK`, ending with `==> all tests passed`. The existing `test_overdub`, `test_budget` and `test_tracks` must pass unchanged, which proves running behaviour is identical.
- [ ] **Step 5: Commit.** `git add plugins/Ouroboros/dsp/Looper.hpp tests/test_transport.cpp && git commit -m "engine: transport — instant stop, park at the top, restart, restart-safe pending ops"`

---

### Task 2: Engine — End of loop and Fade stops, second stop, B during a pending stop

**Files:**
- Modify: `plugins/Ouroboros/dsp/Looper.hpp`
- Test: `tests/test_transport.cpp` (add tests and call them from `main`)

**Interfaces:**
- Consumes: Task 1's `stop`, `restart`, `transport` and loop-gain schedule.
- Produces: no new names. `StopMode::EndOfLoop` and `StopMode::Fade` take effect.

**Behaviour to implement:**
- **End of loop:** with `p = pos_` and `r = L_ - p` at the stop:
  - if `r > ramp_`, wait `r - ramp_` samples, then fade over `ramp_`. It parks exactly at position 0, so no op splits;
  - otherwise fade at once over `max(r, fadeIn_)`. Parking then jumps back to 0 like Instant.
- **Fade:** fade at once over `msToSamples(fadeMs)`, clamped to `[ramp_, 10 s]`, with the same cubic curve.
- **`stop()` while Stopping:**
  - if the loop gain is still above 0, restart the schedule as Instant from the current gain (no wait, `ramp_` long), whatever mode is passed;
  - if the fade has already ended, do nothing.
- **`restart()` while Stopping:**
  - if the mode is EndOfLoop or Fade and the fade hasn't ended, cancel: the transport is Running again, and the loop gain ramps from its current value to 1 over `ramp_`. The position does not jump;
  - otherwise (Instant, or the fade has ended and tails are still running), queue the restart. The park then restarts at once.
- `pressRec` already routes to `restart()` while not Running (Task 1).

- [ ] **Step 1: Write the failing tests:**

```cpp
// out[from + i] against img[(p + i) % L] for i < n.
static float errFrom(const Rig& r, size_t from, const std::vector<float>& img, uint32_t p, uint32_t n) {
    float e = 0.0f; const uint32_t L = uint32_t(img.size());
    for (uint32_t i = 0; i < n; ++i) e = std::max(e, std::fabs(r.out[from + i] - img[(p + i) % L]));
    return e;
}
static void toPosition(Rig& r, uint32_t p) { const uint32_t L = r.lp.loopLength(); r.silence((L + p - r.lp.position()) % L); }
static void endOfLoopStopsAtTheTop() {
    Rig r; baseAndLayer(r); auto img = r.loopImage(); toPosition(r, 6000);
    const uint32_t p = r.lp.position(), L = r.lp.loopLength(); const size_t from = r.out.size();
    r.lp.stop(StopMode::EndOfLoop);
    r.silence(L - p - 720);  CHECK(errFrom(r, from, img, p, L - p - 720) < 1e-6f);   // untouched until 15 ms before the top
    r.silence(720);          CHECK(r.lp.transport() == Transport::Stopped && r.lp.position() == 0);
    CHECK(maxStep(slice(r.out, from - 1, r.out.size())) < kBound);
}
static void endOfLoopKeepsAPendingUndoWhole() {
    Rig r; auto base = baseAndLayer(r); toPosition(r, 6000); r.lp.undo(0);
    const uint32_t L = r.lp.loopLength(); r.lp.stop(StopMode::EndOfLoop);
    r.silence(L - r.lp.position());
    CHECK(r.lp.transport() == Transport::Stopped && r.lp.pendingOps(0) == 1);   // no split
    r.lp.restart(); CHECK(maxAbsDiff(r.settle(), base) < 1e-5f);
}
static void endOfLoopLateStopIsClickFree() {
    Rig r; baseAndLayer(r); toPosition(r, r.lp.loopLength() - 50);   // 50 samples before the top
    const size_t from = r.out.size() - 1; r.lp.stop(StopMode::EndOfLoop); r.silence(4800);
    CHECK(r.lp.transport() == Transport::Stopped && maxStep(slice(r.out, from, r.out.size())) < kBound);
}
// Track 0 holds a constant 0.5, settled. Tail 0 is a 144-sample crossfade: feeding 0.5 through it keeps the start flat.
static void constantLoop(Rig& r) {
    r.lp.setTailMs(0); r.lp.pressRec(0); r.feed(std::vector<float>(24064, 0.5f)); r.lp.pressRec(0);
    r.feed(std::vector<float>(144, 0.5f)); r.settle();
}
static void fadeFollowsTheCurve() {
    Rig r; constantLoop(r); r.lp.stop(StopMode::Fade, 1000.0f);
    auto o = r.capture(48000 + 256);
    CHECK(approx(o[24000], 0.5f * 0.125f, 2e-3));                    // (1 - 0.5)^3
    CHECK(o[12000] > o[24000] && o[24000] > o[36000]);
    CHECK(peak(slice(o, 48001, o.size())) == 0.0f && r.lp.transport() == Transport::Stopped);
    CHECK(maxStep(o) < 1e-3f);
}
static void secondStopCutsAPendingFade() {
    Rig r; constantLoop(r); r.lp.stop(StopMode::Fade, 3000.0f); r.silence(4800);
    const size_t from = r.out.size() - 1;
    r.lp.stop(StopMode::Fade, 3000.0f);                               // a second double-tap: Instant
    r.silence(720 + 128);
    CHECK(r.lp.transport() == Transport::Stopped && maxStep(slice(r.out, from, r.out.size())) < 3e-3f);
}
static void bDuringAFadeCancelsIt() {
    Rig r; constantLoop(r); const uint32_t L = r.lp.loopLength();
    r.lp.stop(StopMode::Fade, 1000.0f); r.silence(24000);
    const uint32_t p = r.lp.position(); r.lp.pressRec(0);
    CHECK(r.lp.transport() == Transport::Running);
    auto o = r.capture(2000);
    CHECK(approx(o[730], 0.5f, 1e-4) && r.lp.position() == (p + 2000) % L && maxStep(o) < 1e-3f);
}
static void bDuringInstantWindowRestartsAfterPark() {
    Rig r; auto in = sine(220, 0.1f, 48000);                          // tail 50 ms
    r.lp.pressRec(0); r.feed(slice(in, 0, 24064)); r.lp.pressRec(0);
    r.lp.stop(StopMode::Instant); r.feed(slice(in, 24064, 25064));   // faded out, tail still writing
    r.lp.pressRec(0); CHECK(r.lp.transport() == Transport::Stopping);   // queued
    r.feed(slice(in, 25064, 27064));                                  // the tail ends at 2400: park, restart
    CHECK(r.lp.transport() == Transport::Running && r.lp.state(0) == ouro::TrackState::Playing);
}
```

- [ ] **Step 2: Run `make test`.** Expected: `test_transport` prints FAIL lines from the new tests. EndOfLoop and Fade still behave as Instant, and restart while Stopping is still ignored.
- [ ] **Step 3: Implement the End of loop and Fade schedules, `stop()` while Stopping and `restart()` while Stopping.**
- [ ] **Step 4: Run `make test`.** Expected: `==> all tests passed`.
- [ ] **Step 5: Commit.** `git commit -am "engine: end-of-loop and fade stops; second stop cuts; B cancels a pending stop"`

---

### Task 3: Engine — auto-record

**Files:**
- Modify: `plugins/Ouroboros/dsp/Constants.hpp` (add `constexpr float kPreRollMs = 10.0f;`) and `plugins/Ouroboros/dsp/Looper.hpp`
- Test: `tests/test_autorec.cpp` (new)

**Interfaces:**
- Consumes: Task 1's `stop` and `Transport`.
- Produces:
  - `void Looper::setAutoRec(bool on);` (default off)
  - `void Looper::setThresholdDb(float db);` clamps to [−60, −10]; NaN goes to −60; the default is −40.
  - `int Looper::armedTrack() const;` returns −1 when nothing is armed.

**Behaviour to implement:**
- **In `pressRec(t)`,** after the transport check:
  - if a track is armed: disarm. If it was `t`, return. Otherwise carry on with `t`'s own action;
  - then close the other writers, as today;
  - then, if `t` is Empty and auto-record is on, arm `t` and return. That covers both the first loop and a track on the loop.
- **Firing the arm:** at the top of each sample in `process()`, before the history write, if a track is armed and `std::fabs(x) >= threshold`:
  - disarm;
  - if `L_ == 0`, start the first recording;
  - else start a recording onto the loop: `startDub(tr, P, false, true, true)`, where P is the pre-roll.
  - In both cases the start is back-dated by `P = msToSamples(kPreRollMs, sr_)`. The sample `x` itself is then recorded by the normal path in the same iteration.
- **`startFirst(t)` becomes `startFirst(int t, uint32_t back)`:**
  - fill `buf[0, back)` from the history, and set `rec_ = back`;
  - `recD_` stays `msToSamples(trigMs_, sr_)` at the start, which is the close's back-date, so `closeFirst` and `recordedSamples()` keep `L = rec_ - recD_`;
  - a manual press passes `back = recD_`, so it is unchanged.
- **Disarm** on: `undo(t)` while anything is armed (disarm and return; a tap on C cancels), `clearTrack(t)` of the armed track, `clearAll()`, `stop()`, and `reset()`.

- [ ] **Step 1: Write the failing tests** in `tests/test_autorec.cpp`:

```cpp
#include "looper_rig.hpp"
using ouro::TrackState;

// `quiet` samples at 0.001 (-60 dB), then n samples of a 0.1 cosine (its first sample, 0.1, crosses -40 dB).
static std::vector<float> quietThenCos(uint32_t quiet, uint32_t n) {
    std::vector<float> x(quiet, 0.001f);
    for (uint32_t i = 0; i < n; ++i) x.push_back(0.1f * float(std::cos(2.0 * M_PI * 220.0 * i / 48000.0)));
    return x;
}
static void armsInsteadOfRecording() {
    Rig r; r.lp.setAutoRec(true); r.lp.pressRec(0);
    CHECK(r.lp.state(0) == TrackState::Empty && r.lp.armedTrack() == 0);
    r.feed(std::vector<float>(4800, 0.001f)); CHECK(r.lp.state(0) == TrackState::Empty);
}
static void startsAtTheCrossingWithPreRoll() {
    Rig r; r.lp.setAutoRec(true); r.lp.pressRec(0); r.feed(quietThenCos(3000, 24000));
    CHECK(r.lp.state(0) == TrackState::Recording && r.lp.armedTrack() == -1);
    r.lp.pressRec(0); CHECK(r.lp.loopLength() == 24000 + 480);      // + the 10 ms pre-roll
    auto img = r.loopImage();
    CHECK(approx(img[480], 0.1f, 1e-6) && approx(img[200], 0.001f, 1e-6));
}
static void closeStaysBackDatedByTriggerLatency() {
    Rig r; r.lp.setTriggerLatencyMs(3.0f); r.lp.setAutoRec(true); r.lp.pressRec(0);
    r.feed(quietThenCos(3000, 24000)); r.lp.pressRec(0);
    CHECK(r.lp.loopLength() == 24000 + 480 - 144);
}
static void armsAnEmptyTrackOnTheLoop() {
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.0f); const uint32_t L = r.lp.loopLength();
    r.lp.setAutoRec(true); r.lp.pressRec(1); CHECK(r.lp.armedTrack() == 1);
    r.feed(std::vector<float>(2000, 0.001f)); CHECK(r.lp.state(1) == TrackState::Empty);
    const uint32_t q = (r.lp.position() + 1000) % L;
    r.feed(quietThenCos(1000, 4800)); CHECK(r.lp.state(1) == TrackState::Recording);
    r.lp.pressRec(1); r.lp.setOn(0, false); auto img = r.settle();
    CHECK(approx(img[q], 0.1f, 1e-5) && approx(img[(q + L - 200) % L], 0.001f, 1e-6));
    CHECK(img[(q + L - 600) % L] == 0.0f);                           // before the pre-roll
}
static void cancelPaths() {
    auto setup = [](Rig& r) { r.lp.setTailMs(0); recordBase(r, 24064, 0.1f); r.lp.setAutoRec(true);
                              r.lp.pressRec(1); return r.lp.armedTrack() == 1; };
    auto nothingStarts = [](Rig& r) { r.feed(quietThenCos(0, 2400)); return r.lp.state(1) == TrackState::Empty; };
    { Rig r; CHECK(setup(r)); r.lp.pressRec(1); CHECK(r.lp.armedTrack() == -1 && nothingStarts(r)); }
    { Rig r; CHECK(setup(r)); r.lp.undo(1);     CHECK(r.lp.armedTrack() == -1 && nothingStarts(r)
                                                      && r.lp.state(0) == TrackState::Playing); }
    { Rig r; CHECK(setup(r)); r.lp.clearTrack(1); CHECK(r.lp.armedTrack() == -1 && nothingStarts(r)); }
    { Rig r; CHECK(setup(r)); r.lp.clearAll();  CHECK(r.lp.armedTrack() == -1); }
    { Rig r; CHECK(setup(r)); r.lp.stop(ouro::StopMode::Instant);
      CHECK(r.lp.armedTrack() == -1 && r.lp.transport() == ouro::Transport::Stopping); }
    { Rig r; CHECK(setup(r)); r.lp.pressRec(0);                       // another track: cancel, then its own action
      CHECK(r.lp.armedTrack() == -1 && r.lp.state(0) == TrackState::Overdubbing && nothingStarts(r)); }
}
static void armingClosesARunningOverdub() {
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.1f);
    r.lp.pressRec(0); r.feed(sine(330, 0.1f, 4800)); r.lp.setAutoRec(true); r.lp.pressRec(1);
    CHECK(r.lp.state(0) == TrackState::Playing && r.lp.undoDepth(0) == 1 && r.lp.armedTrack() == 1);
}
static void armingHandsOverAFirstRecording() {
    Rig r; r.lp.pressRec(0); r.feed(sine(220, 0.1f, 24064));        // a first recording, auto-record off
    r.lp.setAutoRec(true); r.lp.pressRec(1);
    CHECK(r.lp.loopLength() == 24064 && r.lp.state(0) == TrackState::Playing && r.lp.armedTrack() == 1);
}
static void thresholdIsASetting() {
    Rig r; r.lp.setAutoRec(true); r.lp.setThresholdDb(-20.0f); r.lp.pressRec(0);
    r.feed(std::vector<float>(2400, 0.05f)); CHECK(r.lp.state(0) == TrackState::Empty);       // -26 dB
    r.feed(std::vector<float>(128, 0.2f));   CHECK(r.lp.state(0) == TrackState::Recording);   // -14 dB
}
int main() {
    armsInsteadOfRecording(); startsAtTheCrossingWithPreRoll(); closeStaysBackDatedByTriggerLatency();
    armsAnEmptyTrackOnTheLoop(); cancelPaths(); armingClosesARunningOverdub(); armingHandsOverAFirstRecording();
    thresholdIsASetting();
    REPORT();
}
```

- [ ] **Step 2: Run `make test`.** Expected: `test_autorec.cpp` fails to compile (`setAutoRec` is not a member of `ouro::Looper`).
- [ ] **Step 3: Implement the arm, the firing, the `startFirst(t, back)` split and the disarm paths,** as described above.
- [ ] **Step 4: Run `make test`.** Expected: `==> all tests passed`.
- [ ] **Step 5: Commit.** `git add -A plugins/Ouroboros/dsp tests/test_autorec.cpp && git commit -m "engine: auto-record — arm, threshold start with a 10 ms pre-roll"`

---

### Task 4: Plugin — new ports, C gestures, Stop/Play trigger

**Files:**
- Modify: `plugins/Ouroboros/OuroborosPlugin.hpp`
- Test: `tests/test_plugin.cpp`

**Interfaces:**
- Consumes: `Looper::stop(StopMode, float)`, `restart()`, `transport()`, `setAutoRec(bool)`, `setThresholdDb(float)` and `armedTrack()`.
- Produces: the `ParamIndex` entries, inserted between `kParamTriggerLatency` and `kParamBypass` in this order: `kParamStopPlay, kParamStopMode, kParamFadeTime, kParamAutoRec, kParamThreshold, kParamDoubleTap`. Bypass becomes index 30, and `kNumParams` becomes 31.

**Port definitions** (rows for the `defs` table):

```cpp
{"stop_play",  "Stop/Play",  Trig,   0,   1,   0, ""},
{"stop_mode",  "Stop Mode",  Enum,   0,   2,   0, ""},    // "Instant", "End of loop", "Fade"
{"fade_time",  "Fade Time",  Ms,   0.5f, 10,   3, "s"},
{"auto_rec",   "Auto Rec",   Bool,   0,   1,   0, ""},
{"threshold",  "Threshold",  Db,   -60, -10, -40, "dB"},
{"double_tap", "Double-Tap", Ms,   150, 600, 300, "ms"},
```

- **Enum count:** `index == kParamTrack ? 4 : index == kParamStopMode ? 3 : 2`, with the stop mode names above.
- **Groups:** `stop_play` goes in Perform; the other five go in Settings.
- **`run()`:**
  - on change, `setAutoRec(on(kParamAutoRec))` and `setThresholdDb(fVal[kParamThreshold])`;
  - on a `rising(kParamStopPlay)`: if the transport is Running, `stopNow()`, else `fEngine.restart()`;
  - `stopNow()` calls `fEngine.stop(StopMode(clamp(lround(fVal[kParamStopMode]), 0, 2)), fVal[kParamFadeTime] * 1000.0f)`.

**C footswitch** (replaces the undo_redo block). Rename `fRedone` to `fDone`, and add `bool fTapPending = false, fStoppedPress = false; uint64_t fSinceTap = 0; int fTapTrack = 0;`. Reset all of them in `activate()`:

```
hold = hold_time samples; window = double_tap samples       // counted in whole blocks, as today
if undo_redo is on:
    if !fHeld:                                              // press
        fHeld = true; fHold = 0; fDone = false; fStoppedPress = transport == Stopped
        if fTapPending: fTapPending = false; fDone = true; fStoppedPress = false; stopNow()
    fHold += frames
    if !fDone && !fStoppedPress && fHold >= hold:     fDone = true; redo(sel)
    if !fDone &&  fStoppedPress && fHold >= 2 * hold: fDone = true; clearAll()
else if fHeld:                                              // release
    fHeld = false
    if !fDone && fHold < hold: fTapPending = true; fSinceTap = 0; fTapTrack = sel
else if fTapPending && (fSinceTap += frames) >= window:
    fTapPending = false; undo(fTapTrack)
```

- [ ] **Step 1: Write the failing tests:**
  - In `portSymbolsAreFrozen`:
    - extend `kSym` with `"stop_play","stop_mode","fade_time","auto_rec","threshold","double_tap"` and loop to 30;
    - require `kParamBypass == 30 && P().getParameterCount() == 31`;
    - the group check becomes `g == (i <= 8 || i == 24 ? 0u : i <= 16 ? 1u : 2u)`;
    - add `CHECK(P().getParameterEnumValues(kParamStopMode).count == 3 && P().getParameterRanges(kParamFadeTime).def == 3.0f);`
    - add `CHECK(P().getParameterRanges(kParamThreshold).def == -40.0f && P().getParameterRanges(kParamDoubleTap).def == 300.0f && P().getParameterRanges(kParamAutoRec).def == 0.0f);`
  - In `tapUndoHoldRedo`, the two `runN(1)` after a tap release that expect the undo become `runN(114)`.
  - Add these helpers and tests, and call each from `main`:

```cpp
static void tapC() { set(kParamUndoRedo, 1); runN(20); set(kParamUndoRedo, 0); runN(1); }
static void doubleTap() { tapC(); runN(29); set(kParamUndoRedo, 1); runN(1); set(kParamUndoRedo, 0); runN(1); }

static void tapUndoWaitsForTheWindow() {
    fresh(); recordBaseAndDub(); tapC();
    CHECK(eng().undoDepth(0) == 1); runN(110); CHECK(eng().undoDepth(0) == 1);
    runN(3); CHECK(eng().undoDepth(0) == 0);                          // 300 ms after the release
}
static void doubleTapStops() {
    fresh(); recordBaseAndDub(); tapC(); runN(29);
    set(kParamUndoRedo, 1); runN(1); CHECK(eng().transport() == ouro::Transport::Stopping);   // on the press
    runN(20); set(kParamUndoRedo, 0); runN(200);
    CHECK(eng().transport() == ouro::Transport::Stopped && eng().undoDepth(0) == 1 && peak(gOut) == 0.0f);
}
static void bRestartsAfterStop() {
    fresh(); recordBaseAndDub(); doubleTap(); runN(20);
    set(kParamRec, 1); runN(1); set(kParamRec, 0);
    CHECK(eng().transport() == ouro::Transport::Running && eng().state(0) == ouro::TrackState::Playing);
}
static void holdWhileStoppedClearsAll() {
    fresh(); recordBaseAndDub(); tapC(); runN(114); CHECK(eng().canRedo(0));
    doubleTap(); runN(20);
    set(kParamUndoRedo, 1); runN(300); set(kParamUndoRedo, 0); runN(120);   // 800 ms: no redo, no undo
    CHECK(eng().undoDepth(0) == 0 && eng().loopLength() > 0 && eng().transport() == ouro::Transport::Stopped);
    set(kParamUndoRedo, 1); runN(376);                                        // past 1000 ms
    CHECK(eng().loopLength() == 0 && eng().transport() == ouro::Transport::Running);
    set(kParamUndoRedo, 0); runN(120); CHECK(eng().state(0) == ouro::TrackState::Empty);
}
static void doubleTapThenHoldDoesNotClear() {
    fresh(); recordBaseAndDub(); tapC(); runN(29); set(kParamUndoRedo, 1); runN(450);
    CHECK(eng().transport() == ouro::Transport::Stopped && eng().loopLength() > 0);
}
static void tapWhileStoppedUndoes() {
    fresh(); recordBaseAndDub(); doubleTap(); runN(20); tapC(); runN(114);
    CHECK(eng().undoDepth(0) == 0 && eng().transport() == ouro::Transport::Stopped);
}
static void tapUndoesTheTrackItWasTappedOn() {                        // Review Focus 5
    fresh(); recordBaseAndDub();
    set(kParamTrack, 2); runN(1); set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(20);
    set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(40);           // track 2 recorded onto the loop
    set(kParamTrack, 1); runN(1); tapC(); set(kParamTrack, 2); runN(114);
    CHECK(eng().undoDepth(0) == 0 && eng().state(1) == ouro::TrackState::Playing);
}
static void doubleTapDuringAShortFirstRecording() {                   // Review Focus 4
    fresh(); set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(5);
    set(kParamUndoRedo, 1); runN(2); set(kParamUndoRedo, 0); runN(2); set(kParamUndoRedo, 1); runN(1);
    CHECK(eng().state(0) == ouro::TrackState::Empty && eng().loopLength() == 0
          && eng().transport() == ouro::Transport::Running);
    set(kParamUndoRedo, 0); runN(200); CHECK(eng().state(0) == ouro::TrackState::Empty);
}
static void stopPlayPortToggles() {
    fresh(); recordBaseAndDub();
    set(kParamStopPlay, 1); runN(1); set(kParamStopPlay, 0); CHECK(eng().transport() == ouro::Transport::Stopping);
    runN(20); CHECK(eng().transport() == ouro::Transport::Stopped);
    set(kParamStopPlay, 1); runN(1); set(kParamStopPlay, 0); CHECK(eng().transport() == ouro::Transport::Running);
}
static void stopAndAutoRecSettingsReachTheEngine() {
    fresh(); recordBaseAndDub(); set(kParamStopMode, 2); set(kParamFadeTime, 0.5f);
    set(kParamStopPlay, 1); runN(1); set(kParamStopPlay, 0);
    runN(90);  CHECK(eng().transport() == ouro::Transport::Stopping);   // 240 ms into a 500 ms fade
    runN(110); CHECK(eng().transport() == ouro::Transport::Stopped);
    set(kParamStopPlay, 1); runN(1); set(kParamStopPlay, 0);
    set(kParamAutoRec, 1); set(kParamThreshold, -10); set(kParamTrack, 2); runN(1);
    set(kParamRec, 1); runN(5); set(kParamRec, 0);                       // runN's 0.1 sine stays under -10 dB
    CHECK(eng().armedTrack() == 1 && eng().state(1) == ouro::TrackState::Empty);
    set(kParamThreshold, -40); runN(1); CHECK(eng().state(1) == ouro::TrackState::Recording);
}
```

- [ ] **Step 2: Run `make test`.** Expected: `test_plugin.cpp` fails to compile (`kParamStopPlay` was not declared).
- [ ] **Step 3: Implement the ports, the settings wiring, `stopNow()`, the Stop/Play trigger and the C state machine** in `OuroborosPlugin.hpp`.
- [ ] **Step 4: Run `make test` and `make test-lv2`.** Expected: `==> all tests passed`, and `test_lv2_hooks` prints `OK`.
- [ ] **Step 5: Commit.** `git commit -am "plugin: stop/play, stop mode, fade time, auto rec, threshold, double-tap; C double-tap stops, hold clears while stopped"`

---

### Task 5: HMI labels/LEDs and the widget

**Files:**
- Modify: `plugins/Ouroboros/hmi/HmiBridge.hpp`, `plugins/Ouroboros/hmi/LoopWidget.hpp`, `plugins/Ouroboros/OuroborosPlugin.hpp` (fill the new view fields)
- Test: `tests/test_hmi.cpp`, `tests/test_widget.cpp`

**Interfaces:**
- Consumes: `Looper::transport()`, `armedTrack()`.
- Produces:
  - `struct HmiView { TrackState state; NextAction next; bool canUndo; int selected; bool on; Transport transport; bool armed; };`, where `armed` means the selected track is the armed one;
  - `struct WidgetView { ...existing...; Transport transport; bool armed[kTracks]; };`. Zero-initialised means Running and nothing armed.

**Behaviour to implement** (from the spec's Display table):
- **B label:** `ARM` when armed. Otherwise `PLAY` when the transport isn't Running. Otherwise as today.
- **B LED:**
  - armed: red, `LV2_HMI_LED_Blink_Slow` for both on and off;
  - Stopped: `LV2_HMI_LED_Colour_Off`, no blink;
  - Stopping: as today, by track state.
- **The kHmiRec fingerprint** includes `transport` and `armed`, so any change resends.
- **Widget text at (14, textY), tall layout only:**
  - transport Stopped: `STOP`;
  - Stopping: `STOP` drawn only when `blinkOn`;
  - else `armed[selected]`: `ARM`;
  - else the state name, as today.
- **Widget marks:** an Empty track with `armed[i]` draws its 3×3 dot only when `blinkOn`. Everything else is unchanged.
- **Plugin:**
  - `fHmi.update({..., fEngine.transport(), fEngine.armedTrack() == sel})`;
  - `drawWidgets()` sets `v.transport` and `v.armed[t] = fEngine.armedTrack() == t`.

- [ ] **Step 1: Write the failing tests.**
  - In `tests/test_hmi.cpp`:
    - extend the `view()` helper with the trailing parameters `ouro::Transport tp = ouro::Transport::Running, bool armed = false`;
    - add the test below.

```cpp
static void recLabelShowsArmAndStop()
{
    gCalls.clear(); ouro::HmiBridge b; b.setFeature(&gFake); b.addressed(ouro::kHmiRec, A1, kAllCaps);
    b.update(view(ouro::TrackState::Empty, ouro::NextAction::Rec, false, 0, true, ouro::Transport::Running, true));
    CHECK(lastLabel(A1) == "ARM" && lastLed(A1).colour == LV2_HMI_LED_Colour_Red
          && lastLed(A1).on == LV2_HMI_LED_Blink_Slow && lastLed(A1).off == LV2_HMI_LED_Blink_Slow);
    b.update(view(ouro::TrackState::Playing, ouro::NextAction::Dub, true, 0, true, ouro::Transport::Stopping));
    CHECK(lastLabel(A1) == "PLAY" && lastLed(A1).colour == LV2_HMI_LED_Colour_Green && lastLed(A1).on == 0);
    b.update(view(ouro::TrackState::Playing, ouro::NextAction::Dub, true, 0, true, ouro::Transport::Stopped));
    CHECK(lastLabel(A1) == "PLAY" && lastLed(A1).colour == LV2_HMI_LED_Colour_Off);
    b.update(view(ouro::TrackState::Playing, ouro::NextAction::Dub, true, 0, true, ouro::Transport::Running));
    CHECK(lastLabel(A1) == "DUB");
}
```

  - In `tests/test_widget.cpp` (40×37: textY = 3, track 0's dot is x 3..5, y 17..19):

```cpp
// The text band x 14..39, rows 3..9 matches s drawn alone there.
static bool showsText(const ouro::WidgetView& v, const char* s)
{
    uint8_t b[1024], want[1024] = {}; ouro::renderLoopWidget(v, 40, 37, b); ouro::drawText(want, 40, 14, 3, s);
    for (int y = 3; y < 10; ++y) for (int x = 14; x < 40; ++x) if (px(b, 40, x, y) != px(want, 40, x, y)) return false;
    return true;
}
static void stoppedShowsStop()
{
    auto v = view(); v.state[0] = TrackState::Playing; v.loopLength = 1000; v.transport = ouro::Transport::Stopped;
    CHECK(showsText(v, "STOP"));
    v.transport = ouro::Transport::Stopping; v.blinkOn = true;  CHECK(showsText(v, "STOP"));
    v.blinkOn = false; CHECK(showsText(v, ""));
}
static void armedShowsArmAndBlinksItsDot()
{
    auto v = view(); v.armed[0] = true; uint8_t b[1024];
    v.blinkOn = true;  ouro::renderLoopWidget(v, 40, 37, b); CHECK(showsText(v, "ARM") && litIn(b, 40, 3, 17, 6, 20) == 9);
    v.blinkOn = false; ouro::renderLoopWidget(v, 40, 37, b); CHECK(showsText(v, "ARM") && litIn(b, 40, 3, 17, 6, 20) == 0);
}
```

- [ ] **Step 2: Run `make test`.** Expected: `test_hmi.cpp` and `test_widget.cpp` fail to compile (no `transport` member).
- [ ] **Step 3: Implement the view fields, the labels, the LEDs, the fingerprint, the widget text and dot, and fill the fields in the plugin.**
- [ ] **Step 4: Run `make test` and `make test-lv2`.** Expected: `==> all tests passed`, and `test_lv2_hooks` prints `OK`.
- [ ] **Step 5: Commit.** `git commit -am "hmi: ARM/PLAY labels and LEDs; widget shows STOP and ARM"`

---

### Task 6: Web face, manual, README

**Files:**
- Modify: `plugins/Ouroboros/modgui/icon-ouroboros.html`, `plugins/Ouroboros/modgui/script-ouroboros.js`, `plugins/Ouroboros/modgui/stylesheet-ouroboros.css`, `plugins/Ouroboros/modgui.ttl`, `docs/manual/ouroboros-manual.html`, `README.md`
- Regenerate: `plugins/Ouroboros/modgui/screenshot-ouroboros.png` and `thumbnail-ouroboros.png` (`render_screenshot.py`), and `docs/manual/ouroboros-manual.pdf` (`make manual`)

**Interfaces:**
- Consumes: the `stop_play` port symbol (Task 4).

- [ ] **Step 1: Face.**
  - Add `<button type="button" class="ouroboros-key" data-port="stop_play" title="Stop all tracks, or start them again from the top">STOP/PLAY</button>` right after REC.
  - Add `'stop_play'` to `KEYS` in the script, and fix its "six trigger buttons" comment to "seven".
  - Add `modgui:port [ lv2:symbol "stop_play" ; lv2:name "Stop/Play" ] ;` to `modgui.ttl`.
  - Change `.ouroboros-keys` to `grid-template-columns: repeat(4, 1fr);`.
- [ ] **Step 2: Regenerate the screenshot:**
  - run `conda run -n htmd python3 plugins/Ouroboros/modgui/render_screenshot.py`;
  - open the PNG and check that all seven buttons sit inside the pedal frame and that no label overflows its button (two-line labels are fine);
  - if the script writes the thumbnail too, keep it; otherwise leave the thumbnail as it is.
- [ ] **Step 3: Manual (`docs/manual/ouroboros-manual.html`).**
  - **Footswitch B table:** add a row "Stopped | starts every track again from the top | PLAY". Add an "Auto Rec on, track empty | arms it: recording starts on your first note | ARM" row, and a sentence that B or a tap on C cancels the arm.
  - **Footswitch C table:**
    - "Tap" now says the undo happens a moment after you lift your foot (the double-tap window);
    - add "Double-tap | Stops every track (see Stop Mode). Press B to start again from the top.";
    - add "Hold while stopped (twice the hold time) | Clears everything, including the loop length.".
  - **Settings table:**
    - add rows for Stop Mode (Instant / End of loop / Fade, default Instant), Fade Time (0.5–10 s, 3 s), Auto Rec (off/on, off), Threshold (−60…−10 dB, −40 dB: how loud your first note must be to start an armed recording) and Double-Tap (150–600 ms, 300 ms);
    - add the Stop/Play button to the "Knobs and buttons" section.
  - Keep the manual's plain second-person voice.
  - Run `make manual` and check that the PDF has the same page count or one more, with nothing cut off.
- [ ] **Step 4: README.** Change the first bullet to: "Rec / Play / Dub on one footswitch; undo (tap), redo (hold) and stop (double-tap) on another." Add a bullet: "Optional auto-record: arm a track and it starts on your first note."
- [ ] **Step 5: Build and test.** Run `make && make test && make test-lv2`. Expected: the build succeeds, `==> all tests passed`, and `test_lv2_hooks` prints `OK`.
- [ ] **Step 6: Commit.** `git add -A plugins/Ouroboros/modgui plugins/Ouroboros/modgui.ttl docs/manual README.md && git commit -m "face: Stop/Play button; manual and README: stop, clear all, auto-record"`
