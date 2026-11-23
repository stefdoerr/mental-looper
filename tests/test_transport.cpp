#include "looper_rig.hpp"

using ouro::Transport; using ouro::StopMode;

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
static void restartAfterASplitIsClickFree() {                     // the park split: only applied audio, in one pass
    Rig r; baseAndLayer(r); const uint32_t L = r.lp.loopLength();   // the layer is [0, 9600)
    r.silence(L - r.lp.position() + 1000); r.lp.undo(0); r.silence(5000);   // starts at 1000, parks at 6720
    r.lp.stop(StopMode::Instant); r.silence(1000); r.lp.restart();
    CHECK(maxStep(r.capture(L)) < kBound && r.lp.pendingOps(0) == 0);
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
    r.feed(sine(220, 0.1f, 9600)); r.lp.pressRec(0);
    CHECK(peak(r.capture(9600)) > 0.05f);                          // lg_ was reset: the new loop is audible
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

int main() {
    instantStopFadesOutAndParks(); restartPlaysFromTheTop(); pendingUndoSurvivesStopAndRestart();
    zeroOpSurvivesStopAndRestart(); tailFinishesBeforeParking(); stopRestartTwiceWithinAPass();
    restartAfterASplitIsClickFree();
    undoWhileStoppedAppliesFromTheTop(); clearedWhileStoppedResets(); bWhileStoppedOnlyRestarts();
    stopClosesWritersAndNeedsALoop();
    endOfLoopStopsAtTheTop(); endOfLoopKeepsAPendingUndoWhole(); endOfLoopLateStopIsClickFree();
    fadeFollowsTheCurve(); secondStopCutsAPendingFade(); bDuringAFadeCancelsIt(); bDuringInstantWindowRestartsAfterPark();
    REPORT();
}
