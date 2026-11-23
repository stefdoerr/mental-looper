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
static void armEdgeCases() {
    { Rig r; r.lp.setTriggerLatencyMs(50.0f); r.lp.setAutoRec(true); r.lp.pressRec(0);   // close back-date 2400 > 480
      r.feed(quietThenCos(0, 1000)); CHECK(r.lp.recordedSamples() == 0);
      r.lp.pressRec(0); CHECK(r.lp.state(0) == TrackState::Empty && r.lp.loopLength() == 0); }
    { Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.1f); r.lp.clearTrack(0); r.lp.setAutoRec(true); r.lp.pressRec(1);
      r.silence(2000); CHECK(r.lp.loopLength() == 0 && r.lp.armedTrack() == 1);   // L reset by the ramp-out, arm kept
      r.feed(quietThenCos(0, 9600)); r.lp.pressRec(1); CHECK(r.lp.loopLength() == 9600 + 480); }
    { Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.1f); recordBaseOn(r, 1); r.lp.undo(1);   // track 1's base undone
      r.lp.setAutoRec(true); r.lp.pressRec(1); CHECK(r.lp.armedTrack() == 1);
      r.lp.redo(1); CHECK(r.lp.state(1) == TrackState::Playing && r.lp.armedTrack() == -1); }   // not empty: no arm
    { Rig r; r.lp.setAutoRec(true); r.lp.pressRec(0); r.lp.setAutoRec(false);           // Auto Rec off: the arm goes
      CHECK(r.lp.armedTrack() == -1); r.feed(quietThenCos(0, 2400)); CHECK(r.lp.state(0) == TrackState::Empty); }
}
// R6: a fire right after a close must not re-record what the closed writer took. DC 0.2, trigger 3 ms, tail 0 (none
// runs past the close). One loud sample fires the arm, then silence: the seam must read the base's 0.2 alone.
static void armAfterAFirstLoopCloseKeepsTheSeamFlat() {
    Rig r; r.lp.setTailMs(0); r.lp.setTriggerLatencyMs(3.0f);
    r.lp.pressRec(0); r.feed(std::vector<float>(24064, 0.2f));
    r.lp.setAutoRec(true); r.lp.pressRec(1);                         // closes track 0, arms track 1
    std::vector<float> x(4800, 0.0f); x[0] = 0.2f; r.feed(x);         // fires on the next sample
    r.lp.pressRec(1); auto img = r.settle(); const uint32_t L = r.lp.loopLength();
    float e = 0.0f; for (uint32_t i = L - 600; i < L + 144; ++i) e = std::max(e, std::fabs(img[i % L] - 0.2f));
    CHECK(e < 1e-5f);                                                 // doubled: 0.40 at L-100
}
static void armAfterAnOverdubCloseKeepsTheSeamFlat() {
    Rig r; r.lp.setTailMs(0); r.lp.setTriggerLatencyMs(3.0f); recordBase(r, 24064, 0.0f);
    const uint32_t L = r.lp.loopLength();
    r.lp.pressRec(0); r.feed(std::vector<float>(4800, 0.2f)); const uint32_t c = r.lp.position();
    r.lp.setAutoRec(true); r.lp.pressRec(1);                         // closes the overdub at c, arms track 1
    std::vector<float> x(4800, 0.0f); std::fill(x.begin(), x.begin() + 330, 0.005f); x[330] = 0.2f;
    r.feed(x);                                                        // fires 6.9 ms after the press, past the fade
    r.lp.pressRec(1); auto img = r.settle();
    float e = 0.0f; for (uint32_t i = 1; i <= 1000; ++i) e = std::max(e, std::fabs(img[(c + L - i) % L] - 0.2f));
    CHECK(e < 1e-5f);                                                 // doubled: 0.39 at c-10
}
int main() {
    armsInsteadOfRecording(); startsAtTheCrossingWithPreRoll(); closeStaysBackDatedByTriggerLatency();
    armsAnEmptyTrackOnTheLoop(); cancelPaths(); armingClosesARunningOverdub(); armingHandsOverAFirstRecording();
    thresholdIsASetting(); armEdgeCases();
    armAfterAFirstLoopCloseKeepsTheSeamFlat(); armAfterAnOverdubCloseKeepsTheSeamFlat();
    REPORT();
}
