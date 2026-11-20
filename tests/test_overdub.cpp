#include "looper_rig.hpp"

static void undoRestoresExactly() {
    Rig r; r.lp.setTailMs(0); auto base = recordBase(r, 24064, 0.1f);
    r.lp.pressRec(0); r.feed(sine(330, 0.1f, 9600)); r.lp.pressRec(0);
    auto dubbed = r.settle();  CHECK(maxAbsDiff(dubbed, base) > 0.05f && r.lp.undoDepth(0) == 1);
    r.lp.undo(0);  CHECK(maxAbsDiff(r.settle(), base) < 1e-5f);
    r.lp.redo(0);  CHECK(maxAbsDiff(r.settle(), dubbed) < 1e-5f);
}
static void undoIsLazy() {
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.1f);
    r.lp.pressRec(0); r.feed(sine(330, 0.1f, 24064)); r.lp.pressRec(0); r.settle();
    r.lp.undo(0); r.silence(128);  CHECK(r.lp.pendingOps(0) == 1);
    r.silence(24064);              CHECK(r.lp.pendingOps(0) == 0);
}
static void undoIsClickFree() {
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.1f);
    r.lp.pressRec(0); r.feed(sine(330, 0.3f, 30000)); r.lp.pressRec(0); r.settle();
    r.lp.undo(0);
    CHECK(maxStep(r.capture(9600)) < 2.5f * (sineStep(330, 0.3f) + sineStep(220, 0.1f)));
}
static void undoRampsSpanThePress() {                               // the ramps, measured across each press
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.1f);
    r.lp.pressRec(0); r.feed(sine(330, 0.3f, 30000)); r.lp.pressRec(0); r.settle();
    r.silence(5000); const size_t from = r.out.size() - 100;       // mid-layer, where the delta is loud
    r.lp.undo(0); r.silence(2000);                                  // the layer
    r.lp.undo(0);                                                   // the base: the track goes Empty
    CHECK(r.lp.state(0) == ouro::TrackState::Empty && r.lp.canRedo(0));
    r.silence(2000);                                                // (the last track: then L resets, redo goes)
    CHECK(peak(slice(r.out, r.out.size() - 1000, r.out.size())) == 0.0f);
    CHECK(maxStep(slice(r.out, from, r.out.size())) < 2.5f * (sineStep(330, 0.3f) + sineStep(220, 0.1f)));
}
static void undoWhileOverdubbingCancels() {
    Rig r; r.lp.setTailMs(0); auto base = recordBase(r, 24064, 0.1f);
    r.lp.pressRec(0); r.feed(sine(330, 0.1f, 4800)); r.lp.undo(0);
    CHECK(r.lp.state(0) == ouro::TrackState::Playing && !r.lp.canRedo(0));
    CHECK(maxAbsDiff(r.settle(), base) < 1e-5f);
}
static void onePressOneLayer() {
    Rig r; r.lp.setTailMs(0); auto base = recordBase(r, 24064, 0.1f);
    r.lp.pressRec(0); r.feed(sine(330, 0.1f, 60160)); r.lp.pressRec(0);   // 2.5 passes
    CHECK(r.lp.undoDepth(0) == 1); r.lp.undo(0); CHECK(maxAbsDiff(r.settle(), base) < 1e-5f);
}
static void silentOverdubMakesNoLayer() {
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.1f);
    r.lp.pressRec(0); r.feed(std::vector<float>(9600, 1e-5f)); r.lp.pressRec(0); r.settle();
    CHECK(r.lp.undoDepth(0) == 0);
    r.lp.pressRec(0); r.feed(sine(330, 0.01f, 9600)); r.lp.pressRec(0);
    CHECK(r.lp.undoDepth(0) == 1);
}
static void punchesAreClickFree() {
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.0f);       // silent loop
    auto s = sine(330, 0.3f, 9877); r.lp.pressRec(0); r.feed(slice(s, 37, 9637)); r.lp.pressRec(0);
    r.feed(slice(s, 9637, 9877));                                // keep playing through the 5 ms punch-out fade
    CHECK(maxStep(wrap(r.settle())) < 2.5f * sineStep(330, 0.3f));
}
static void recToDubUndoIsSeamless() {
    Rig r; r.lp.setRecToDub(true); auto in = sine(220, 0.1f, 48000);
    r.lp.pressRec(0); r.feed(slice(in, 0, 24064)); r.lp.pressRec(0);
    CHECK(r.lp.state(0) == ouro::TrackState::Overdubbing);
    r.feed(slice(in, 24064, 33664)); r.lp.pressRec(0); r.lp.undo(0);
    CHECK(maxStep(wrap(r.settle())) < 2.5f * sineStep(220, 0.1f));
}
static void recToDubTailZeroUndoIsSeamless() {                     // undoing the dub leaves the 3 ms crossfade
    for (float trig : {0.0f, 3.0f}) {
        Rig r; r.lp.setRecToDub(true); r.lp.setTailMs(0); r.lp.setTriggerLatencyMs(trig);
        auto in = sine(220, 0.3f, 60000);
        r.lp.pressRec(0); r.feed(slice(in, 0, 24100)); r.lp.pressRec(0);
        r.feed(slice(in, 24100, 30000)); r.lp.pressRec(0); r.feed(slice(in, 30000, 30240));
        CHECK(maxStep(wrap(r.settle())) < 2.5f * sineStep(220, 0.3f));
        r.lp.undo(0); CHECK(maxStep(wrap(r.settle())) < 2.5f * sineStep(220, 0.3f));
    }
}
static void recToDubSplitsTheBackDatedTail(float advanceMs) {       // D = 96 < T = 2400: fold + live tail
    Rig r; r.lp.setRecToDub(true); r.lp.setTriggerLatencyMs(2.0f); r.lp.setAdvanceMs(advanceMs);   // no offset: it continues
    const uint32_t s = 4096, D = 96, T = 2400, L = 24064;
    auto in = sine(220, 0.1f, 48000);
    r.feed(slice(in, 0, s)); r.lp.pressRec(0); r.feed(slice(in, s, s + L)); r.lp.pressRec(0);
    r.feed(slice(in, s + L, s + L + 9600)); r.lp.pressRec(0); r.feed(slice(in, s + L + 9600, s + L + 9840));
    auto img = r.loopImage(); float unity = 0.0f, base = 0.0f;
    for (uint32_t p = 0; p < T; ++p) unity = std::max(unity, std::fabs(img[p] - in[s - D + p] * fadeInGain(p) - in[s - D + L + p]));
    r.lp.undo(0); img = r.settle();
    for (uint32_t p = 0; p < T; ++p)
        base = std::max(base, std::fabs(img[p] - in[s - D + p] * fadeInGain(p) - in[s - D + L + p] * float(T - p) / float(T)));
    CHECK(unity < 1e-6f && base < 1e-6f);
}
static void redubRightAfterCancelFadesIn() {                        // the cancelled layer leaves no split behind
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.0f); r.lp.setTriggerLatencyMs(2.0f);
    auto x = sine(330, 0.3f, 9840);
    r.lp.pressRec(0); r.feed(slice(x, 0, 4836)); r.lp.undo(0); r.lp.pressRec(0);   // 4836: a peak
    r.feed(slice(x, 4836, 9600)); r.lp.pressRec(0); r.feed(slice(x, 9600, 9840));
    CHECK(maxStep(wrap(r.settle())) < 2.5f * sineStep(330, 0.3f));
}
static void dubMidTailFadesIn() {                                   // Rec->Play, DUB at g = 0.5: no step at q
    Rig r; auto in = sine(220, 0.1f, 48000);                       // tail 50 ms = 2400
    r.lp.pressRec(0); r.feed(slice(in, 0, 24064)); r.lp.pressRec(0);
    r.feed(slice(in, 24064, 25264)); r.lp.pressRec(0);             // DUB at position 1200
    r.feed(slice(in, 25264, 33664)); r.lp.pressRec(0); r.feed(slice(in, 33664, 33904));
    CHECK(maxStep(wrap(r.settle())) < 2.5f * sineStep(220, 0.1f));
}
static void dubLateInPunchOutFadeFadesIn() {                        // DUB 200 samples into the 240-sample fade
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.0f);          // silent loop
    auto s = sine(330, 0.3f, 9840);
    r.lp.pressRec(0); r.feed(slice(s, 0, 4800)); r.lp.pressRec(0);
    r.feed(slice(s, 4800, 5000)); r.lp.pressRec(0);
    r.feed(slice(s, 5000, 9600)); r.lp.pressRec(0); r.feed(slice(s, 9600, 9840));
    CHECK(r.lp.undoDepth(0) == 2 && maxStep(wrap(r.settle())) < 2.5f * sineStep(330, 0.3f));
}
// D > 0: a DUB back-dated into a punch-out fade or into the first recording's tail takes only the complement there.
static void backDatedDubIntoFadeOrTail(float trigMs) {
    for (uint32_t into : {50u, 200u, 239u}) {                      // DUB this far into the 240-sample punch-out fade
        Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.0f); r.lp.setTriggerLatencyMs(trigMs);   // silent loop
        auto s = sine(330, 0.3f, 9840);
        r.lp.pressRec(0); r.feed(slice(s, 0, 4800)); r.lp.pressRec(0);
        r.feed(slice(s, 4800, 4800 + into)); r.lp.pressRec(0);
        r.feed(slice(s, 4800 + into, 9600)); r.lp.pressRec(0); r.feed(slice(s, 9600, 9840));
        CHECK(r.lp.undoDepth(0) == 2 && maxStep(wrap(r.settle())) < 2.5f * sineStep(330, 0.3f));
        r.lp.undo(0); CHECK(maxStep(wrap(r.settle())) < 2.5f * sineStep(330, 0.3f));
    }
    for (uint32_t at : {100u, 1200u, 2300u}) {                     // DUB this far into the 2400-sample tail
        Rig r; r.lp.setTriggerLatencyMs(trigMs); auto in = sine(220, 0.1f, 48000);
        r.feed(slice(in, 0, 1000)); r.lp.pressRec(0); r.feed(slice(in, 1000, 25064)); r.lp.pressRec(0);
        r.feed(slice(in, 25064, 25064 + at)); r.lp.pressRec(0);
        r.feed(slice(in, 25064 + at, 34664)); r.lp.pressRec(0); r.feed(slice(in, 34664, 34904));
        CHECK(maxStep(wrap(r.settle())) < 2.5f * sineStep(220, 0.1f));
        r.lp.undo(0); CHECK(maxStep(wrap(r.settle())) < 2.5f * sineStep(220, 0.1f));
    }
}
// A 2 ms burst overdubbed at position 4000 of a silent loop; returns the first loop position it reaches.
static int onsetWith(float advanceMs) {
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.0f); r.lp.setAdvanceMs(advanceMs);
    r.silence(4000); r.lp.pressRec(0); r.feed(sine(1000, 0.3f, 96)); r.lp.pressRec(0);
    auto img = r.settle();
    for (size_t i = 0; i < img.size(); ++i) if (std::fabs(img[i]) > 1e-3f) return int(i);
    return -1;
}
static void advanceWritesEarlier() {
    // same 2 ms burst overdubbed twice on a silent loop, with offset 0 and 1 ms; the onset moves 48 samples earlier
    CHECK(std::abs(onsetWith(0.0f) - onsetWith(1.0f) - 48) <= 1);
}
static void newOverdubDiscardsRedo() {
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.1f);
    r.lp.pressRec(0); r.feed(sine(330, 0.1f, 4800)); r.lp.pressRec(0); r.lp.undo(0); r.settle();
    r.lp.pressRec(0); r.feed(sine(440, 0.1f, 4800)); r.lp.pressRec(0);
    CHECK(!r.lp.canRedo(0));
}
static void undoMashIsBounded() {                                   // Review Focus 3
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.1f);
    r.lp.pressRec(0); r.feed(sine(330, 0.1f, 9600)); r.lp.pressRec(0); auto dubbed = r.settle();
    for (int i = 0; i < 40; ++i) { if (i % 2) r.lp.redo(0); else r.lp.undo(0); r.silence(128);
                                   CHECK(r.lp.pendingOps(0) <= ouro::kMaxPendingOps); }
    CHECK(maxAbsDiff(r.settle(), dubbed) < 1e-5f);                 // even count ends on redo
}
static void capBlocksOnlyLayerUndoRedo() {                          // at the op cap: base undo/redo and a cancel still work
    Rig r; r.lp.setTailMs(0); auto base = recordBase(r, 24064, 0.1f);
    auto x = sine(330, 0.1f, 64);
    for (uint32_t i = 0; i < ouro::kMaxPendingOps; ++i) { r.lp.pressRec(0); r.feed(x); r.lp.undo(0); }   // 32 cancels
    CHECK(r.lp.pendingOps(0) == ouro::kMaxPendingOps);
    r.lp.undo(0); CHECK(r.lp.state(0) == ouro::TrackState::Empty && r.lp.canRedo(0));   // the base: no op slot
    r.lp.redo(0); CHECK(r.lp.state(0) == ouro::TrackState::Playing);
    r.lp.pressRec(0); r.feed(sine(440, 0.1f, 4800)); r.lp.undo(0);   // cancel
    CHECK(r.lp.state(0) == ouro::TrackState::Playing && r.lp.undoDepth(0) == 0 && !r.lp.canRedo(0));
    CHECK(maxAbsDiff(r.settle(), base) < 1e-5f);
}

int main() {
    undoRestoresExactly(); undoIsLazy(); undoIsClickFree(); undoRampsSpanThePress(); undoWhileOverdubbingCancels();
    onePressOneLayer(); silentOverdubMakesNoLayer(); punchesAreClickFree(); recToDubUndoIsSeamless(); recToDubTailZeroUndoIsSeamless();
    recToDubSplitsTheBackDatedTail(0.0f); recToDubSplitsTheBackDatedTail(5.0f); redubRightAfterCancelFadesIn(); dubMidTailFadesIn(); dubLateInPunchOutFadeFadesIn();
    backDatedDubIntoFadeOrTail(2.0f); backDatedDubIntoFadeOrTail(3.0f);
    advanceWritesEarlier(); newOverdubDiscardsRedo(); undoMashIsBounded(); capBlocksOnlyLayerUndoRedo();
    REPORT();
}
