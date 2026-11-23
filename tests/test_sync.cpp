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

// After a tempo change the close does not match the next writer's back-date: that writer fades in under what is left,
// with no step, whether it is another track's recording or the Rec->Dub layer. DC in, so any step is a click.
static std::vector<float> afterTempoChange(double spb2, uint32_t late, bool handover, bool toDub, bool alone = true) {
    SyncRig r; r.lp.setSync(true); r.lp.setRecToDub(toDub);
    const std::vector<float> dc(300000, 0.1f);
    r.lp.pressRec(0); r.feedSync(slice(dc, 0, 96000));
    if (spb2 != 96000.0) { r.spb = spb2; r.phase = 0.0; }               // the tempo changes on a line
    r.feedSync(slice(dc, 0, uint32_t(spb2) + late)); r.lp.pressRec(handover ? 1 : 0);   // close, 'late' after the line
    const uint32_t L = r.lp.loopLength();
    r.feedSync(slice(dc, 0, L / 2));
    if (handover || toDub) r.lp.pressRec(handover ? 1 : 0);             // close the new writer
    r.feedSync(slice(dc, 0, 2000));                                     // the input goes on: its punch-out fades it
    if (handover && alone) r.lp.setOn(0, false);                        // track 1 alone
    r.silenceSync(L + 20000);
    return r.loopImage();
}
static void theNextWriterFadesInAfterATempoChange() {
    const float smooth = 0.002f;                                         // DC 0.1 over a 3 ms fade: 0.0007 a sample
    for (uint32_t late : {0u, 1280u}) {
        CHECK(maxStep(wrap(afterTempoChange(96000.0, late, true, false))) < smooth);   // control: no tempo change
        CHECK(maxStep(wrap(afterTempoChange(80000.0, late, true, false))) < smooth);   // rounded down: 2 bars
        CHECK(maxStep(wrap(afterTempoChange(60000.0, late, true, false))) < smooth);   // rounded up: 3 bars
        for (double spb2 : {80000.0, 60000.0}) {                        // the Rec->Dub layer alone
            const auto a = afterTempoChange(spb2, late, false, true), b = afterTempoChange(spb2, late, false, false);
            std::vector<float> layer(a.size());
            for (size_t i = 0; i < a.size(); ++i) layer[i] = a[i] - b[i];
            CHECK(peak(layer) > 0.09f && maxStep(wrap(layer)) < smooth);
        }
    }
    const auto mix = afterTempoChange(60000.0, 1280, true, false, false);   // rounded up, track 0 still on
    CHECK(peak(slice(mix, 156000, 157280)) < 0.101f);                    // the handed-over stretch is not taken twice
}

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
// A restart within the grace while MOD plays starts at once, every playhead advanced as if it had started on the line.
static void aLateRestartPlaysAsIfStartedOnTheLine() {
    SyncRig r; r.lp.setSync(true); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 96000)); r.lp.pressRec(0); r.silenceSync(20000);
    const auto img = r.loopImage();                                      // one bar: MOD's phase is the same after it
    r.lp.stop(ouro::StopMode::Instant); r.silenceSync(2000); CHECK(r.lp.transport() == ouro::Transport::Stopped);
    r.toPhase(1280); r.lp.pressRec(0);                                   // 26.7 ms after the line
    CHECK(!r.lp.waiting() && r.lp.transport() == ouro::Transport::Running && r.lp.position() == 1280);
    const auto o = r.capture(2000);                                      // past the 3 ms fade-in: the loop from 1280
    CHECK(r.lp.trackPosition(0) == 1280 + 2000);
    CHECK(maxAbsDiff(slice(o, 144, 2000), slice(img, 1280 + 144, 1280 + 2000)) < 1e-6f);
}
// The advance splits a pending op as a park does: an undo made while stopped is heard from the late restart on.
static void aLateRestartKeepsAPendingUndo() {
    SyncRig r; r.lp.setSync(true); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 96000)); r.lp.pressRec(0); r.silenceSync(20000);
    const auto base = r.loopImage();
    r.lp.pressRec(0); r.feedSync(sine(480, 0.1f, 96000)); r.lp.pressRec(0); r.silenceSync(2000);   // a layer everywhere
    r.lp.stop(ouro::StopMode::Instant); r.silenceSync(2000); r.lp.undo(0);                          // undone, stopped
    r.toPhase(1280); r.lp.pressRec(0);
    const auto o = r.capture(2000);
    CHECK(maxAbsDiff(slice(o, 144, 2000), slice(base, 1280 + 144, 1280 + 2000)) < 1e-5f);
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

// Beyond the brief: auto-record's first loop on MOD's grid. An early crossing waits for the line (the arm becomes the
// waiting action); the pre-roll is a lead-in: the loop's top is on the line, the pre-roll wraps to its end, no step.
static void autoRecordFirstLoopKeepsThePreRollAsALeadIn() {
    SyncRig r; r.lp.setSync(true); r.lp.setTailMs(0); r.lp.setAutoRec(true); r.silenceSync(128);
    r.lp.pressRec(0); CHECK(r.lp.armedTrack() == 0);
    const auto burst = sine(240, 0.1f, 400);                            // two cycles; the line falls on a trough
    r.toPhase(96000 - 150); r.feedSync(slice(burst, 0, 100));           // crosses 146 samples before the line
    CHECK(r.lp.waiting() && r.lp.waitingTrack() == 0 && r.lp.armedTrack() == -1 && r.lp.state(0) == TrackState::Empty);
    r.feedSync(slice(burst, 100, 400)); CHECK(!r.lp.waiting() && r.lp.state(0) == TrackState::Recording);
    r.silenceSync(96000 - 250 + 640); r.lp.pressRec(0);                 // 13 ms after the next line
    CHECK(r.lp.loopLength() == 96000 && r.lp.bars() == 1);
    const auto img = r.loopImage();
    CHECK(maxAbsDiff(slice(img, 96000 - 150, 96000), slice(burst, 0, 150)) < 1e-6f);   // the pre-roll, at the end
    CHECK(maxAbsDiff(slice(img, 0, 250), slice(burst, 150, 400)) < 1e-6f);             // from the line, not faded in
    CHECK(maxStep(wrap(img)) < 1.01f * sineStep(240, 0.1f));
}
// Beyond the brief: an arm onto the loop that crosses within the grace keeps everything from the line on, and the
// pre-roll before it (the brief's test above passes either way).
static void autoRecordOntoTheLoopKeepsFromTheLine() {
    SyncRig r; r.lp.setSync(true); r.lp.setTailMs(0); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(std::vector<float>(96000, 0.0f)); r.lp.pressRec(0);   // a silent 1-bar loop
    r.lp.setAutoRec(true); r.lp.pressRec(1); r.toPhase(96000 - 480);
    const auto quiet = sine(240, 0.005f, 480 + 1280);                   // under the threshold: pre-roll and bar start
    r.feedSync(quiet); r.feedSync(std::vector<float>(1000, 0.2f)); r.lp.pressRec(1);   // crosses 1280 after the line
    const auto img = r.loopImage();
    CHECK(maxAbsDiff(slice(img, 96000 - 480 + 144, 96000), slice(quiet, 144, 480)) < 1e-7f);   // past its fade-in
    CHECK(maxAbsDiff(slice(img, 0, 1280), slice(quiet, 480, 1760)) < 1e-7f);
}
// Beyond the brief: B during an Instant stop restarts at the park, which (MOD playing) then waits for the line.
static void aQueuedRestartWaitsForTheLine() {
    SyncRig r; r.lp.setSync(true); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 96000)); r.lp.pressRec(0); r.silenceSync(20000);
    r.lp.stop(ouro::StopMode::Instant); r.lp.pressRec(0); r.silenceSync(2000);
    CHECK(r.lp.transport() == ouro::Transport::Stopped && r.lp.waiting() && r.lp.waitingTrack() == -1);
    r.silenceSync(96000 - 22000 + 128); CHECK(r.lp.transport() == ouro::Transport::Running);
}
// Beyond the brief (Task 1's parked finding): the max length closes the first loop on the last line passed; a press
// waiting on another track survives it and starts that track on the next line.
static void maxLengthKeepsAPressWaitingOnAnotherTrack() {
    SyncRig r(1000.0); r.spb = 960.0; r.lp.setSync(true); r.silenceSync(960);   // 1 kHz: max length 300000
    r.lp.pressRec(0); r.silenceSync(299700);                            // 180 samples past line 312: past the grace
    r.lp.pressRec(1); CHECK(r.lp.waiting() && r.lp.waitingTrack() == 1);
    r.silenceSync(600);                                                 // the max length, then 180 short of line 313
    CHECK(r.lp.loopLength() == 312 * 960 && r.lp.waiting());
    r.silenceSync(300); CHECK(!r.lp.waiting() && r.lp.state(1) == TrackState::Recording);
}

// Task 3: following MOD's play/stop. Each retrigger jumps off-phase (not a whole number of periods from the top), so
// a hard cut would step: without the crossfade these measure 29x (one track, overdub) and 58x (two tracks) sineStep.
static void modStartRetriggersOnTheLine() {
    SyncRig r; r.lp.setSync(true); r.lp.setTailMs(0); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(240, 0.1f, 96000)); r.lp.pressRec(0);   // 240 Hz: whole cycles in a bar
    r.silenceSync(40064); const size_t from = r.out.size() - 1;
    r.phase = 0.0; r.lp.modStarted(true); r.silenceSync(4800);
    CHECK(r.lp.position() < 4800 + 128 && r.lp.trackPosition(0) < 4800 + 128);   // back near the top
    CHECK(maxStep(slice(r.out, from, r.out.size())) < 2.5f * 2.0f * sineStep(240, 0.1f));   // no click
}
static void modStartWhileOverdubbingKeepsTheLayer() {                    // Review Focus 3
    SyncRig r; r.lp.setSync(true); r.lp.setTailMs(0); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(240, 0.1f, 96000)); r.lp.pressRec(0);
    auto base = r.loopImage();
    r.lp.pressRec(0); r.feedSync(sine(480, 0.1f, 20064)); const size_t from = r.out.size() - 1;
    r.phase = 0.0; r.lp.modStarted(true); r.silenceSync(2000);
    CHECK(maxStep(slice(r.out, from, r.out.size())) < 2.0f * (sineStep(240, 0.1f) + sineStep(480, 0.1f)));   // no click
    CHECK(r.lp.state(0) == TrackState::Playing && r.lp.undoDepth(0) == 1);
    r.lp.undo(0); CHECK(maxAbsDiff(r.settle(), base) < 1e-5f);
}
static void modStartOverTracksOfOneAndTwoBars() {                       // the 2-bar track lands on its beginning
    SyncRig r; r.lp.setSync(true); r.lp.setTailMs(0); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(240, 0.1f, 96000)); r.lp.pressRec(0);
    r.toPhase(96000 - 1280); r.lp.pressRec(1); r.silenceSync(1280);        // starts on the line
    r.feedSync(sine(330, 0.1f, 2 * 96000)); r.lp.pressRec(1); CHECK(r.lp.trackLength(1) == 2 * 96000);
    r.silenceSync(50064); const size_t from = r.out.size() - 1;
    r.phase = 0.0; r.lp.modStarted(true); r.silenceSync(4800);
    CHECK(r.lp.position() == 4800 && r.lp.trackPosition(1) == 4800);
    CHECK(maxStep(slice(r.out, from, r.out.size())) < 2.0f * (sineStep(240, 0.1f) + sineStep(330, 0.1f)));
}
static void modStartMidBarWaitsForTheLine() {                            // Review Focus 4
    SyncRig r; r.lp.setSync(true); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 96000)); r.lp.pressRec(0); r.silenceSync(10000);
    r.phase = 30000.0; const uint32_t p = r.lp.position();
    r.lp.modStarted(false); r.silenceSync(128);
    CHECK(r.lp.position() == (p + 128) % 96000 && r.lp.waiting());     // no jump yet
    r.silenceSync(66000 - 128 + 128); CHECK(!r.lp.waiting() && r.lp.position() <= 128);
}
// userWaiting: a press on t or a restart is the user's wait; MOD's lone retrigger is not.
static void userWaitingIsAPressAnArmOrARestartNotARetrigger() {
    SyncRig r; r.lp.setSync(true); r.toPhase(96000 - 1280);
    CHECK(!r.lp.userWaiting(0));
    r.lp.pressRec(0); CHECK(r.lp.userWaiting(0) && !r.lp.userWaiting(1));
    SyncRig q; q.lp.setSync(true); q.toPhase(0);
    q.lp.pressRec(0); q.feedSync(sine(220, 0.1f, 96000)); q.lp.pressRec(0); q.silenceSync(10000);
    q.phase = 30000.0; q.lp.modStarted(false); q.silenceSync(128);
    CHECK(q.lp.waiting() && !q.lp.userWaiting(0));                     // retrigger alone
    SyncRig s; s.lp.setSync(true); s.toPhase(0);
    s.lp.pressRec(0); s.feedSync(sine(220, 0.1f, 96000)); s.lp.pressRec(0); s.silenceSync(20000);
    s.lp.stop(ouro::StopMode::Instant); s.silenceSync(2000);
    s.toPhase(48000); s.lp.pressRec(0);
    CHECK(s.lp.userWaiting(0) && s.lp.userWaiting(2));                 // a restart waits for every track
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
static void modStartFromStoppedRestarts() {                              // tail 0: the default 50 ms parks after 2432
    SyncRig r; r.lp.setSync(true); r.lp.setTailMs(0); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 96000)); r.lp.pressRec(0);
    r.lp.modStopped(ouro::StopMode::Instant, 3000.0f); r.silenceSync(2000);
    CHECK(r.lp.transport() == ouro::Transport::Stopped);
    r.phase = 0.0; r.lp.modStarted(true); r.silenceSync(128);
    CHECK(r.lp.transport() == ouro::Transport::Running && r.lp.position() <= 128);
}

// Beyond the brief: a retrigger's old positions hear their pending ops (applied ahead): an undo still going through
// the loop doesn't bring the undone layer back for the crossfade.
static void aRetriggerHearsAPendingUndo() {
    SyncRig r; r.lp.setSync(true); r.lp.setTailMs(0); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(240, 0.1f, 96000)); r.lp.pressRec(0);
    r.lp.pressRec(0); r.feedSync(sine(330, 0.1f, 96000)); r.lp.pressRec(0); r.silenceSync(2000);   // a layer everywhere
    r.lp.undo(0); r.silenceSync(20000); const size_t from = r.out.size() - 1;   // halfway through, at the layer's peak
    r.phase = 0.0; r.lp.modStarted(true); r.silenceSync(4800);
    CHECK(maxStep(slice(r.out, from, r.out.size())) < 2.5f * 2.0f * sineStep(240, 0.1f));
}
// Beyond the brief: a retrigger only moves the playheads. A recording onto the loop it closes (started on a line, so
// its punch-out fade lies past the close, where a zero op clears the rest) and a first loop's tail it cuts into keep
// exactly what a press, or no MOD start, would have left.
static std::vector<float> ontoTheLoopClosedBy(bool retrig) {
    SyncRig r; r.lp.setSync(true); r.lp.setTailMs(0); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(240, 0.1f, 96000)); r.lp.pressRec(0);
    r.toPhase(96000 - 1280); r.lp.pressRec(1); r.silenceSync(1280);        // starts on the line, back-date 0
    const auto in = sine(330, 0.1f, 42000);
    r.feedSync(slice(in, 0, 40000));                                       // under a loop: rounded up
    if (retrig) { r.phase = 0.0; r.lp.modStarted(true); } else r.lp.pressRec(1);
    r.feedSync(slice(in, 40000, 42000));                                   // the punch-out fade takes the input on
    return r.settle();
}
static std::vector<float> tailCutBy(bool retrig) {
    SyncRig r; r.lp.setSync(true); r.toPhase(0);                           // the default 50 ms tail
    const auto in = sine(240, 0.1f, 100000);
    r.lp.pressRec(0); r.feedSync(slice(in, 0, 96000)); r.lp.pressRec(0);
    r.feedSync(slice(in, 96000, 97280));
    if (retrig) { r.phase = 0.0; r.lp.modStarted(true); }
    r.feedSync(slice(in, 97280, 100000));                                  // the tail takes the input on to its end
    return r.settle();
}
static void aRetriggerLeavesTheLoopsAsTheyWere() {
    CHECK(maxAbsDiff(ontoTheLoopClosedBy(true), ontoTheLoopClosedBy(false)) < 1e-6f);
    CHECK(maxAbsDiff(tailCutBy(true), tailCutBy(false)) < 1e-6f);
}
// Beyond the brief: MOD's mid-bar start never cancels a waiting press, nor a press it: both go on the line, the
// retrigger first. Ignored with every track empty and during a first recording.
static void aModStartJoinsAWaitingPress() {
    SyncRig r; r.lp.setSync(true); r.lp.setTailMs(0); r.toPhase(0);
    r.lp.modStarted(false); CHECK(!r.lp.waiting());                       // nothing to start
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 50000));
    r.lp.modStarted(false); CHECK(!r.lp.waiting() && r.lp.state(0) == TrackState::Recording);
    r.feedSync(sine(220, 0.1f, 46000)); r.lp.pressRec(0); r.silenceSync(10000);
    r.lp.pressRec(1); CHECK(r.lp.waitingTrack() == 1);                   // 10000 after the line: waits
    r.phase = 30000.0; r.lp.modStarted(false); CHECK(r.lp.waitingTrack() == 1);
    r.lp.pressRec(1); CHECK(r.lp.waiting() && r.lp.waitingTrack() == -1);   // the press goes, the retrigger stays
    r.lp.pressRec(1); CHECK(r.lp.waitingTrack() == 1);
    r.silenceSync(66000 + 128);
    CHECK(!r.lp.waiting() && r.lp.position() <= 128);
    CHECK(r.lp.state(1) == TrackState::Recording && r.lp.trackPosition(1) <= 128);
}
// Beyond the brief (Task 2's parked finding): a restart queued during an Instant stop, late within the grace at the
// park, plays on MOD's timeline, not a sample behind it.
static void aQueuedRestartPlaysOnModsTimeline() {
    SyncRig r; r.lp.setSync(true); r.lp.setTailMs(0); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 96000)); r.lp.pressRec(0); r.silenceSync(1280);   // pos_ = MOD's
    r.lp.stop(ouro::StopMode::Instant); r.lp.pressRec(0); r.silenceSync(1280);   // parks ~2000 after the line
    CHECK(r.lp.transport() == ouro::Transport::Running && !r.lp.waiting());
    CHECK(r.lp.position() == uint32_t(r.phase));
}

int main() {
    earlyPressWaitsForTheLine(); latePressIsBackDatedToTheLine(); pressPastTheGraceWaitsForTheNextLine();
    firstLoopIsWholeBars(); tempoChangeDuringTheFirstRecording(); cancelAWaitingPress(); noGridActsAtOnce();
    syncOffIsUnchanged(); freeGridStartsAtTheFirstPress();
    aFreeGridRecordingKeepsItsGrid(); stopClosesOnTheLastLine(); roundedUpPastTheRecordingIsSilence();
    fractionalBarsKeepTheSeam(); theNextWriterFadesInAfterATempoChange();
    recordingOntoTheLoopStartsOnALine(); autoRecordStartsOnALine(); restartWaitsForTheLineWhileModPlays();
    aLateRestartPlaysAsIfStartedOnTheLine(); aLateRestartKeepsAPendingUndo();
    syncTurnedOnOverAFreeLoop();
    autoRecordFirstLoopKeepsThePreRollAsALeadIn(); autoRecordOntoTheLoopKeepsFromTheLine();
    aQueuedRestartWaitsForTheLine();
    maxLengthKeepsAPressWaitingOnAnotherTrack();
    modStartRetriggersOnTheLine(); modStartWhileOverdubbingKeepsTheLayer(); modStartOverTracksOfOneAndTwoBars();
    modStartMidBarWaitsForTheLine();
    modStopDuringTheFirstRecordingKeepsWholeBars(); modStartFromStoppedRestarts();
    aRetriggerHearsAPendingUndo(); aRetriggerLeavesTheLoopsAsTheyWere(); aModStartJoinsAWaitingPress();
    aQueuedRestartPlaysOnModsTimeline(); userWaitingIsAPressAnArmOrARestartNotARetrigger();
    REPORT();
}
