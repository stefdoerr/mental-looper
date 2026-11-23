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

// Beyond the brief: the spec's "the first recording keeps the grid it started on", the stop's close, and L rounded up
// past what was recorded.
static void aFreeGridRecordingKeepsItsGrid() {                           // MOD starts during it: its own lines still count
    SyncRig r; r.lp.setSync(true); r.playing = false; r.silenceSync(128);
    r.lp.pressRec(0); CHECK(r.lp.state(0) == TrackState::Recording);    // bar 1 at the press
    r.feedSync(sine(220, 0.1f, 50000)); r.playing = true; r.phase = 30000.0;   // MOD starts mid-bar
    r.feedSync(sine(220, 0.1f, 96000 - 50000 + 640)); r.lp.pressRec(0); // 13 ms after the recording's bar 2, mid-bar for MOD
    CHECK(!r.lp.waiting() && r.lp.loopLength() == 96000 && r.lp.bars() == 1);
}
static void stopClosesOnTheLastLine() {                                  // whole bars; under one bar is dropped
    SyncRig r; r.lp.setSync(true); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 96000 + 30000)); r.lp.stop(ouro::StopMode::Instant);
    CHECK(r.lp.loopLength() == 96000 && r.lp.bars() == 1);
    SyncRig q; q.lp.setSync(true); q.toPhase(0);
    q.lp.pressRec(0); q.feedSync(sine(220, 0.1f, 50000)); q.lp.stop(ouro::StopMode::Instant);
    CHECK(q.lp.loopLength() == 0 && q.lp.state(0) == TrackState::Empty);
}
static void roundedUpPastTheRecordingIsSilence() {                       // a tempo change can make L outgrow the recording
    SyncRig r; r.lp.setTailMs(0);
    r.lp.pressRec(0); r.feedSync(sine(330, 0.1f, 200000)); r.lp.pressRec(0);   // Sync off: leaves old audio in track 0
    r.lp.clearAll(); r.silenceSync(2000); CHECK(r.lp.loopLength() == 0);
    r.lp.setSync(true); r.toPhase(0);
    const auto in = sine(225, 0.1f, 156000);                             // ends at its peak: an unfaded end would click
    r.lp.pressRec(0); r.feedSync(slice(in, 0, 96000));
    r.spb = 60000.0; r.phase = 0.0;                                      // the tempo changes on a line
    r.feedSync(slice(in, 96000, 156000)); r.lp.pressRec(0);              // round(156000 / 60000) = 3 bars of 60000
    CHECK(r.lp.bars() == 3 && r.lp.loopLength() == 180000);
    const auto img = r.loopImage();
    CHECK(peak(slice(img, 156000, 180000)) == 0.0f);                     // the unrecorded rest is silence
    CHECK(maxStep(wrap(img)) < 2.0f * sineStep(225, 0.1f));              // no click where the recording ends
}

static void fractionalBarsKeepTheSeam() {                                // L is the line-to-line span, not round(bars x spb)
    SyncRig r; r.spb = 96000.6; r.lp.setSync(true); r.lp.setTailMs(0);  // lines at 192001.2 and 288001.8: span 96000
    const std::vector<float> dc(96282, 0.1f);                            // flat: a 3 ms crossfade keeps it flat
    r.silenceSync(2 * 96000 - 1280); r.lp.pressRec(0); CHECK(r.lp.waiting());   // early for bar 3
    r.feedSync(dc); r.lp.pressRec(0); CHECK(r.lp.waiting());            // early for bar 4: closes there, back-date 0
    r.feedSync(slice(dc, 0, 1300)); CHECK(r.lp.loopLength() == 96000 && r.lp.bars() == 1);
    const auto img = r.loopImage();
    CHECK(*std::min_element(img.begin(), img.end()) > 0.099f);          // no dip at the seam
}

int main() {
    earlyPressWaitsForTheLine(); latePressIsBackDatedToTheLine(); pressPastTheGraceWaitsForTheNextLine();
    firstLoopIsWholeBars(); tempoChangeDuringTheFirstRecording(); cancelAWaitingPress(); noGridActsAtOnce();
    syncOffIsUnchanged(); freeGridStartsAtTheFirstPress();
    aFreeGridRecordingKeepsItsGrid(); stopClosesOnTheLastLine(); roundedUpPastTheRecordingIsSilence();
    fractionalBarsKeepTheSeam();
    REPORT();
}
