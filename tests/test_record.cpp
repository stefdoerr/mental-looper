#include "looper_rig.hpp"

static void recordSetsLengthAndPlays() {
    Rig r; auto in = sine(220, 0.1f, 24064);
    r.lp.pressRec(0); r.feed(in); r.lp.pressRec(0);
    CHECK(r.lp.loopLength() == 24064);
    CHECK(r.lp.state(0) == ouro::TrackState::Playing && r.lp.nextAction(0) == ouro::NextAction::Dub);
}
static void loopPlaysRecordedAudio() {
    Rig r; r.lp.setTailMs(0); auto in = sine(220, 0.1f, 24064);
    r.lp.pressRec(0); r.feed(in); r.lp.pressRec(0);
    auto img = r.loopImage();
    for (uint32_t i = 0; i < 24064; ++i) CHECK(std::fabs(img[i] - in[i] * fadeInGain(i)) < 1e-6f);
}
static void backDatingKeepsLengthAndShiftsStart() {
    Rig r; r.lp.setTailMs(0); r.lp.setTriggerLatencyMs(2.0f);       // D = 96
    auto in = sine(220, 0.1f, 48000);
    r.feed(slice(in, 0, 4096)); r.lp.pressRec(0); r.feed(slice(in, 4096, 28160)); r.lp.pressRec(0);
    CHECK(r.lp.loopLength() == 24064);
    auto img = r.loopImage();
    CHECK(maxAbsDiff(slice(img, 200, 20000), slice(in, 4000 + 200, 4000 + 20000)) < 1e-6f);
}
static void seamIsContinuous() {
    Rig r; auto in = sine(220, 0.1f, 48000);                       // tail 50 ms (default)
    r.lp.pressRec(0); r.feed(slice(in, 0, 24064)); r.lp.pressRec(0);
    r.feed(slice(in, 24064, 24064 + 4864));                        // keep playing through the tail
    CHECK(maxStep(wrap(r.loopImage())) < 2.5f * sineStep(220, 0.1f));
}
static void tailZeroSeamIsContinuous() {                            // tail 0 is still a 3 ms crossfade
    Rig r; r.lp.setTailMs(0); auto in = sine(220, 0.3f, 48000);
    r.lp.pressRec(0); r.feed(slice(in, 0, 24100)); r.lp.pressRec(0);   // closed mid-wave
    r.feed(slice(in, 24100, 24100 + 4800));
    CHECK(maxStep(wrap(r.loopImage())) < 2.5f * sineStep(220, 0.3f));
}
static void backDatedTailFoldMatchesAnalytic() {                    // R9: fold of the D samples + live tail
    Rig r; r.lp.setTriggerLatencyMs(2.0f);                          // D = 96, tail 50 ms (default): T = 2400
    const uint32_t s = 4096, D = 96, T = 2400, L = 24064;           // s = index of the start press
    auto in = sine(220, 0.1f, 48000);
    r.feed(slice(in, 0, s)); r.lp.pressRec(0); r.feed(slice(in, s, s + L)); r.lp.pressRec(0);
    r.feed(slice(in, s + L, s + L + 4864));                         // keep playing through the tail
    CHECK(r.lp.loopLength() == L);
    auto img = r.loopImage();
    float err = 0.0f;
    for (uint32_t p = 0; p < L; ++p) {
        float e = in[s - D + p] * fadeInGain(p);
        if (p < T) e += in[s - D + L + p] * float(T - p) / float(T);
        err = std::max(err, std::fabs(img[p] - e));
    }
    CHECK(err < 1e-6f);
}
static void tailClampedOnShortLoop() {
    Rig r; r.lp.setTailMs(500); auto in = sine(220, 0.1f, 48000);
    r.lp.pressRec(0); r.feed(slice(in, 0, 7168)); r.lp.pressRec(0); r.feed(slice(in, 7168, 31168));
    auto img = r.loopImage();
    CHECK(r.lp.loopLength() == 7168);
    CHECK(maxStep(wrap(img)) < 2.5f * sineStep(220, 0.1f) && peak(img) < 0.25f);
}
static void tooShortIsAborted() {
    Rig r; r.lp.pressRec(0); r.feed(sine(220, 0.1f, 4736)); r.lp.pressRec(0);   // 98.7 ms
    CHECK(r.lp.loopLength() == 0 && r.lp.state(0) == ouro::TrackState::Empty);
}
static void undoWhileRecordingAborts() {
    Rig r; r.lp.pressRec(0); r.feed(sine(220, 0.1f, 24064)); r.lp.undo(0);
    CHECK(r.lp.state(0) == ouro::TrackState::Empty && r.lp.loopLength() == 0);
    CHECK(peak(r.capture(4800)) == 0.0f);
}
static void elapsedCountsWhileRecording() {
    Rig r; r.lp.pressRec(0); r.silence(4864); CHECK(r.lp.recordedSamples() == 4864);
}
static void maxLengthAutoCloses() {
    Rig r(8000.0); r.lp.pressRec(0); r.silence(8000 * 301);
    CHECK(r.lp.state(0) == ouro::TrackState::Playing && r.lp.loopLength() == 8000u * 300u);
}
static void initFailureIsSafe() {                                   // Review Focus 4
    ouro::Looper lp; CHECK(!lp.init(1.0e12, 128)); CHECK(!lp.enabled());
    float in[128], out[128]; std::fill(in, in + 128, 0.3f);
    lp.pressRec(0); lp.process(in, out, 128);
    CHECK(out[0] == 0.0f && lp.state(0) == ouro::TrackState::Empty);
}

int main() {
    recordSetsLengthAndPlays(); loopPlaysRecordedAudio(); backDatingKeepsLengthAndShiftsStart();
    seamIsContinuous(); tailZeroSeamIsContinuous(); backDatedTailFoldMatchesAnalytic(); tailClampedOnShortLoop(); tooShortIsAborted(); undoWhileRecordingAborts();
    elapsedCountsWhileRecording(); maxLengthAutoCloses(); initFailureIsSafe();
    REPORT();
}
