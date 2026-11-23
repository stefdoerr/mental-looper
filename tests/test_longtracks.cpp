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
static void leadInLongerThanTheLoopWrapsAround() {                     // back = 100 ms latency + 30 ms Offset > L
    const uint32_t Ls = 4800, D = 4800, a = 1440, P = 6000;            // the shortest loop, 100 ms
    for (uint32_t n : {2000u, 4900u}) {                                // rounded up, rounded down: one loop
        Rig r; r.lp.setTailMs(0); recordBase(r, Ls, 0.0f); r.silence(1234);
        r.lp.setTriggerLatencyMs(100.0f); r.lp.setAdvanceMs(30.0f);
        auto x = sine(317, 0.1f, P + n);
        r.feed(slice(x, 0, P)); const uint32_t phase = r.lp.position();
        r.lp.pressRec(1); r.feed(slice(x, P, P + n)); r.lp.pressRec(1); r.silence(1000);
        std::vector<float> want(Ls, 0.0f);                             // input j lands a early, faded in from P - D
        for (uint32_t j = P - D; j < P + n; ++j) want[(j + 2 * Ls + phase - P - a) % Ls] += x[j] * fadeInGain(j - (P - D));
        CHECK(r.lp.trackLength(1) == Ls && maxAbsDiff(trackImage(r, 1), want) < 1e-6f);
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

// R7: a close whose playhead lands on recorded audio mid-wave ramps the track in, no step. Block 1: the press lands
// exactly. The base is silent and the input stops at the close, so the output is the track alone.
static float stepAround(const Rig& r, size_t at) { return maxStep(slice(r.out, at - 1, at + 2000)); }
static void aCloseLandingMidWaveRampsIn() {
    const float bound = 2.5f * sineStep(220, 0.1f);
    for (int closer : {1, 2}) {                                        // B, a handover: 1.17 loops rounds down
        Rig r(48000.0, 1); silentBase(r); toPos(r, 6000);
        r.lp.pressRec(1); r.feed(sine(220, 0.1f, 28000)); r.lp.pressRec(closer);
        const size_t at = r.out.size(); r.silence(2000);
        CHECK(r.lp.trackLength(1) == L); CHECK(stepAround(r, at) < bound);
    }
    {                                                                  // rounded up into the lead-in: 72 samples before
        Rig r(48000.0, 1); silentBase(r); r.lp.setTriggerLatencyMs(3.0f);   // two loops, the lead-in is 144
        auto x = sine(220, 0.1f, 1000 + 2 * L - 72);
        toPos(r, 6000 - 1000); r.feed(slice(x, 0, 1000));
        r.lp.pressRec(1); r.feed(slice(x, 1000, x.size())); r.lp.pressRec(1);
        const size_t at = r.out.size(); r.silence(2000);
        CHECK(r.lp.trackLength(1) == 2 * L); CHECK(stepAround(r, at) < bound);
    }
    {                                                                  // the maximum, 8 kHz: 342 loops of 7000, onto index 0
        Rig r(8000.0, 1); r.lp.setTailMs(0); recordBase(r, 7000, 0.0f); r.lp.setTriggerLatencyMs(3.0f);
        const uint32_t pre = 1009, max = 342 * 7000;                   // the press on a crest; the lead-in fades in before it
        auto x = sine(220, 0.1f, pre + max, 8000.0);
        r.feed(slice(x, 0, pre)); r.lp.pressRec(1); r.feed(slice(x, pre, x.size()));
        const size_t at = r.out.size(); r.silence(2000);
        CHECK(r.lp.trackLength(1) == max); CHECK(stepAround(r, at) < 2.5f * sineStep(220, 0.1f, 8000.0));
    }
}

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
static void tiedLongestKeepTheirOffset() {                             // begun 3 passes apart: the lower lands on 0
    Rig r; silentBase(r); recordLoops(r, 1, 6000, 2, 220); recordLoops(r, 2, 3000, 2, 330);
    r.silence(41234); r.lp.stop(StopMode::Instant); r.silence(1000);
    CHECK(r.lp.trackPosition(1) == 0 && r.lp.trackPosition(2) == L);
}
static void endOfLoopWaitsForATrackTheStopCloses() {                   // 2 loops + 2000: closed by the stop, 2 loops
    Rig r; silentBase(r); toPos(r, 6000); r.lp.pressRec(1); r.feed(sine(220, 0.1f, 2 * L + 2000));
    r.lp.stop(StopMode::EndOfLoop);
    const uint32_t rest = 2 * L - r.lp.trackPosition(1);
    r.silence(rest - 1); CHECK(r.lp.transport() == Transport::Stopping);
    r.silence(1); CHECK(r.lp.transport() == Transport::Stopped && r.lp.trackPosition(1) == 0);
}
static void manyOpsThroughShiftingParks() {                            // 2 loops under 3: alternate parks land L apart
    Rig r; silentBase(r); recordLoops(r, 1, 6000, 2, 220); recordLoops(r, 2, 15000, 3, 330);
    auto img0 = trackImage(r, 1);
    for (int i = 0; i < 25; ++i) { r.lp.pressRec(1); r.feed(sine(550, 0.1f, 1000)); r.lp.undo(1); }   // 25 cancel ops
    for (int i = 0; i < 4; ++i) { r.silence(3000); r.lp.stop(StopMode::Instant); r.silence(1000); r.lp.restart(); }
    r.lp.pressRec(0); r.feed(sine(330, 0.1f, 2 * L)); r.lp.pressRec(0);   // reuses the blocks the cancels freed
    r.silence(4 * L);
    CHECK(maxAbsDiff(trackImage(r, 1), img0) < 1e-5f && r.lp.pendingOps(1) == 0);
}
// The cancel starts at s with track 2 at 2L - 2860. Park 1 lands track 1 at s + L + 2860: the delta's reader owes
// [s + L + 2860, s + 2L), the owner [s + 5720, s + L + 2860). Park 2 lands it L on, at s + 2860: the owner ends first.
static void anOwnerEndingFirstHandsItsDeltaOn() {
    Rig r; silentBase(r); recordLoops(r, 1, 6000, 2, 220); recordLoops(r, 2, 15000, 3, 330);
    auto img0 = trackImage(r, 1);
    r.silence((5 * L - 3860 - r.lp.trackPosition(2)) % (3 * L));
    r.lp.pressRec(1); r.feed(sine(550, 0.1f, 1000)); r.lp.undo(1);     // a cancel op, owning its delta
    for (int i = 0; i < 2; ++i) { r.silence(5000); r.lp.stop(StopMode::Instant); r.silence(1000); r.lp.restart(); }
    r.silence(L + 100); r.lp.pressRec(0); r.feed(sine(330, 0.1f, 4096)); r.lp.pressRec(0);   // takes a freed block
    r.silence(2 * L);
    CHECK(maxAbsDiff(trackImage(r, 1), img0) < 1e-5f);
}

int main() {
    oneLoopTracksReportLengthAndPosition();
    longRecordingRoundsUp(); longRecordingRoundsDownOntoTheStart(); oneLoopFromMidPassStaysOneLoop(); lessThanALoopIsOneLoop();
    roundingCountsFromTheStartPress(); bStopsAtThePress(); handoverAndStopCloseWithTheRule(); maximumClosesAtWholeLoops();
    reusedBufferNeverPlaysOldAudio(); leadInSitsBeforeTheStartOrWrapsToTheEnd(); leadInLongerThanTheLoopWrapsAround();
    autoRecordCanMakeALongTrack();
    recToDubAfterALongCloseUndoesExactly(); longTrackSeamIsContinuous(); aCloseLandingMidWaveRampsIn();
    overdubUndoRedoOnALongTrack(); pendingUndoOnALongTrackSurvivesStop(); parkPutsTheLongestOnItsBeginning();
    endOfLoopWaitsForTheLongest(); twoLongTracksKeepTheirAlignment(); baseUndoRedoOfALongTrackKeepsAlignment();
    tiedLongestKeepTheirOffset(); endOfLoopWaitsForATrackTheStopCloses(); manyOpsThroughShiftingParks();
    anOwnerEndingFirstHandsItsDeltaOn();
    REPORT();
}
