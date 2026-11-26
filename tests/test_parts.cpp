#include "looper_rig.hpp"
using ouro::TrackState;

// Parts: 4 parts of 4 tracks, switched at the current part's end; an empty part is filled from the one left (shared).

static void switchWaitsForThePartEnd() {
    Rig r; const auto a = recordBase(r, 24000, 0.1f);
    r.silence(6000); r.lp.setPart(1);
    CHECK(r.lp.part() == 0 && r.lp.queuedPart() == 1);
    r.silence(24000 - r.lp.position() - 1); CHECK(r.lp.part() == 0);
    r.silence(2); CHECK(r.lp.part() == 1 && r.lp.queuedPart() == -1);
    CHECK(maxAbsDiff(r.settle(), a) < 1e-6f);                            // the copy plays what A played
}
static void copyIsIndependentAndKeepsOnAndVolume() {
    Rig r; const auto a = recordBase(r, 24000, 0.1f);
    recordBaseOn(r, 1); r.lp.setOn(1, false); r.lp.setVolumeDb(0, -6.0f);
    r.lp.setOn(2, false); r.lp.setVolumeDb(2, -12.0f);                   // R6: an empty track's settings follow too
    r.lp.pressRec(0); r.feed(sine(330, 0.05f, 24000)); r.lp.pressRec(0); r.silence(4800);   // A: 2 layers on track 0
    r.lp.setPart(1); r.silence(24000);
    CHECK(r.lp.part() == 1 && !r.lp.isOn(1) && r.lp.volumeDb(0) == -6.0f);
    CHECK(!r.lp.isOn(2) && r.lp.volumeDb(2) == -12.0f);
    CHECK(r.lp.layerCount(0) == 1 && r.lp.canUndo(0) && !r.lp.canRedo(0));
    r.lp.pressRec(0); r.feed(sine(660, 0.05f, 24000)); r.lp.pressRec(0); r.silence(4800);   // B changes
    const auto b = r.settle();
    r.lp.setPart(0); r.silence(24000);
    CHECK(r.lp.layerCount(0) == 2 && maxAbsDiff(r.settle(), b) > 0.01f);   // A did not get B's overdub
}
static void copyOnEntryAllocatesNothing() {
    Rig r; recordBase(r, 24000, 0.1f); recordBaseOn(r, 1);
    const uint32_t used = r.lp.blocksUsed();
    r.lp.setPart(1); r.silence(24000);
    CHECK(r.lp.part() == 1 && r.lp.blocksUsed() == used);
}
static void partLengthIsTheLongestTrack() {
    Rig r; recordBase(r, 24000, 0.1f);
    r.lp.pressRec(1); r.feed(sine(330, 0.1f, 3 * 24000 - r.lp.position())); r.lp.pressRec(1);   // a 3-loop track
    CHECK(r.lp.partLength() == 3 * 24000);
    r.lp.setPart(1);
    const uint32_t left = 3 * 24000 - r.lp.trackPosition(1);
    r.silence(left - 1); CHECK(r.lp.part() == 0);
    r.silence(2); CHECK(r.lp.part() == 1);
}
static void choosingTheCurrentPartCancels() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.setPart(2); r.lp.setPart(0);
    CHECK(r.lp.queuedPart() == -1); r.silence(48000); CHECK(r.lp.part() == 0);
}
static void stoppedSwitchesAtOnce() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.stop(ouro::StopMode::Instant); r.silence(2000);
    r.lp.setPart(1); CHECK(r.lp.part() == 1 && r.lp.state(0) == TrackState::Playing);   // filled on entry
}
static void firstRecordingDefersTheSwitch() {
    Rig r; r.lp.pressRec(0); r.feed(sine(220, 0.1f, 12000)); r.lp.setPart(1);
    CHECK(r.lp.part() == 0 && r.lp.queuedPart() == 1);
    r.feed(sine(220, 0.1f, 12000)); r.lp.pressRec(0);                      // L = 24000
    r.silence(23000); CHECK(r.lp.part() == 0);
    r.silence(2000); CHECK(r.lp.part() == 1);
}
static void aRunningTakeClosesAndTheCopyHasItsPunchOut() {
    Rig r; recordBase(r, 24000, 0.1f); r.silence(24000 - r.lp.position() + 6000);
    r.lp.pressRec(0); r.lp.setPart(1);                                    // overdubbing when the part ends
    r.feed(sine(330, 0.05f, 24000));
    CHECK(r.lp.part() == 1 && r.lp.state(0) == TrackState::Playing && r.lp.layerCount(0) == 1);
    const auto b = r.settle();
    r.lp.setPart(0); r.silence(24000);
    CHECK(r.lp.layerCount(0) == 2 && maxAbsDiff(r.settle(), b) < 1e-6f);   // same audio, punch-out fade included
}
static void noClickAtTheSwitch() {
    Rig r; recordBase(r, 24000, 0.2f);
    r.lp.setPart(1); r.silence(24000); r.lp.clearTrack(0); r.silence(4800);
    recordBaseOn(r, 0, 0.2f, 880.0);                                     // B: a different tone
    for (int k = 0; k < 4; ++k) { r.lp.setPart(k % 2 ? 1 : 0); r.silence(24000); }
    CHECK(maxStep(slice(r.out, r.out.size() - 4 * 24000, r.out.size())) <= 1.5f * sineStep(880.0, 0.2f));
}
static void undoOnACopiedTrack() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.setPart(1); r.silence(24000);
    r.lp.undo(0); r.silence(4800); CHECK(r.lp.state(0) == TrackState::Empty);
    r.lp.redo(0); r.silence(4800); CHECK(r.lp.state(0) == TrackState::Playing);
}
static void clearedPartRefillsOnEntry() {
    Rig r; const auto a = recordBase(r, 24000, 0.1f);
    r.lp.setPart(1); r.silence(24000); r.lp.clearTrack(0); r.silence(4800);
    CHECK(r.lp.loopLength() == 24000);                                    // A still holds a track: L stays
    r.lp.setPart(0); r.silence(24000); r.lp.setPart(1); r.silence(24000);
    CHECK(r.lp.part() == 1 && maxAbsDiff(r.settle(), a) < 1e-6f);
}
static void lResetsOnlyWhenEveryPartIsEmpty() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.setPart(1); r.silence(24000);
    r.lp.clearTrack(0); r.silence(4800); CHECK(r.lp.loopLength() == 24000);
    r.lp.setPart(0); r.silence(24000); r.lp.clearTrack(0); r.silence(4800);
    CHECK(r.lp.loopLength() == 0);
}
static void clearAllEmptiesEveryPartAndGoesToA() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.setPart(2); r.silence(24000);
    r.lp.clearAll(); r.silence(4800);
    CHECK(r.lp.part() == 0 && r.lp.loopLength() == 0 && r.lp.blocksUsed() == 0);
}
static void anEmptyPartLastsOneLoop() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.setPart(1); r.silence(24000);
    r.lp.setPart(0); r.silence(24000); r.lp.clearTrack(0); r.silence(4800);   // A: no tracks, B: one
    r.lp.setPart(1); r.silence(24000 + 128); CHECK(r.lp.part() == 1);
}
static void modStartKeepsAQueuedSwitch() {                               // Sync on, MOD playing (see test_sync's SyncRig)
    SyncRig r; r.lp.setSync(true); r.lp.setTailMs(0); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 96000)); r.lp.pressRec(0);
    r.silenceSync(30000); r.lp.setPart(1);
    r.lp.modStarted(true);                                               // MOD restarts on a line
    r.silenceSync(1000); CHECK(r.lp.part() == 0 && r.lp.queuedPart() == 1);
    r.silenceSync(96000); CHECK(r.lp.part() == 1);
}
// Beyond the brief: an undo still owing most of its pass at the switch reaches the copy, and A finishes it on its
// return. The copy's share of the delta is its own: nothing leaks once everything is cleared.
static void aPendingUndoReachesTheCopy() {
    Rig r; const auto a = recordBase(r, 24000, 0.1f);
    r.lp.pressRec(0); r.feed(sine(330, 0.05f, 24000)); r.lp.pressRec(0); r.silence(19200);
    r.lp.undo(0); r.lp.setPart(1); r.silence(4800);                      // the undo has done [19200, 24000)
    CHECK(r.lp.part() == 1 && r.lp.pendingOps(0) == 1);
    CHECK(maxAbsDiff(r.settle(), a) < 1e-5f);
    recordBaseOn(r, 1);                                                  // B takes blocks: none of the undo's are free
    r.lp.setPart(0); r.silence(24000);
    CHECK(r.lp.part() == 0 && maxAbsDiff(r.settle(), a) < 1e-5f && r.lp.canRedo(0));
    r.lp.clearAll(); r.silence(4800); CHECK(r.lp.loopLength() == 0 && r.lp.blocksUsed() == 0);
}
// Beyond the brief: into a copy, the switch is seamless: B ramps in as A ramps out, on the same audio. Track 1 starts
// mid-wave at its beginning (recorded from 4800), so a missing ramp on either side shows.
static void theSwitchIntoACopyIsSeamless() {
    auto run = [](bool sw) {
        Rig r; recordBase(r, 24000, 0.1f); r.silence(4800); recordBaseOn(r, 1, 0.2f, 471.0);
        if (sw) r.lp.setPart(1);
        r.silence(24000); return slice(r.out, r.out.size() - 24000, r.out.size());
    };
    CHECK(maxAbsDiff(run(true), run(false)) < 1e-4f);
}
// Beyond the brief: a part left while stopped is frozen at once, silent, so clearing everything still resets L.
static void aPartLeftWhileStoppedLetsLReset() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.setPart(1); r.silence(24000);
    r.lp.stop(ouro::StopMode::Instant); r.silence(2000); r.lp.setPart(0); r.lp.restart(); r.silence(4800);
    r.lp.clearAll(); r.silence(4800);
    CHECK(r.lp.part() == 0 && r.lp.loopLength() == 0 && r.lp.blocksUsed() == 0);
}
// M1: a first recording that ends without a loop (aborted, cleared, too short) leaves nothing recorded, so a switch
// queued during it happens at once.
static void aFirstRecordingEndingWithoutALoopSwitchesAtOnce() {
    Rig r; r.lp.pressRec(0); r.feed(sine(220, 0.1f, 12000)); r.lp.setPart(1);
    r.lp.undo(0); CHECK(r.lp.part() == 1 && r.lp.queuedPart() == -1);                  // aborted
    r.lp.pressRec(0); r.feed(sine(220, 0.1f, 12000)); r.lp.setPart(2);
    r.lp.clearTrack(0); CHECK(r.lp.part() == 2 && r.lp.queuedPart() == -1);            // cleared
    r.lp.pressRec(0); r.feed(sine(220, 0.1f, 1000)); r.lp.setPart(3);
    r.lp.pressRec(0);                                                                   // under kMinLoopMs: dropped
    CHECK(r.lp.part() == 3 && r.lp.queuedPart() == -1 && r.lp.loopLength() == 0);
}
// M2: Clear All inside a switch's crossfade: the part left keeps ramping out (a freeze would snap it), the part cleared
// ramps out too, and L then resets in part A.
static void clearAllInsideTheSwitchCrossfadeDoesNotClick() {
    Rig r; recordBase(r, 24000, 0.1f); r.silence(4800); recordBaseOn(r, 1, 0.2f, 471.0);   // track 1 mid-wave at the top
    r.lp.setPart(1); r.silence(24000 - r.lp.position() + 64);                          // 64 samples into the crossfade
    CHECK(r.lp.part() == 1);
    const size_t from = r.out.size() - 1;
    r.lp.clearAll(); r.silence(4800);
    CHECK(r.lp.part() == 0 && r.lp.loopLength() == 0);
    CHECK(maxStep(slice(r.out, from, r.out.size())) <= 1.5f * (sineStep(220.0, 0.1f) + sineStep(471.0, 0.2f)));
}
// R5: the switch crossfades over the restart's fade-in (kFadeInMs), not kRampMs: past it only the new part sounds.
static void theSwitchCrossfadesOverTheFadeIn() {
    Rig r; recordBase(r, 24000, 0.1f);
    r.lp.setPart(1); r.silence(24000); r.lp.clearTrack(0); r.silence(4800);
    recordBaseOn(r, 0, 0.2f, 880.0);                                     // B: its own tone
    r.lp.setPart(0); r.silence(24000 - r.lp.position());                 // the switch back to A, at B's end
    const auto in = r.capture(24000), steady = r.capture(24000);         // both from A's top
    const uint32_t f = ouro::msToSamples(ouro::kFadeInMs, 48000.0);
    CHECK(maxAbsDiff(slice(in, 0, f), slice(steady, 0, f)) > 1e-3f);     // B fading out under A
    CHECK(maxAbsDiff(slice(in, f, 2000), slice(steady, f, 2000)) < 1e-6f);
}
// M4: a song step on the part already playing is no switch: the part plays on exactly as without the song.
static void aSongStepOnTheCurrentPartIsNoSwitch() {
    auto run = [](bool song) {
        Rig r; recordBase(r, 24000, 0.1f); r.silence(4800); recordBaseOn(r, 1, 0.2f, 471.0);
        r.silence(24000 - r.lp.position());                              // A's part end
        r.lp.setStep(0, 0, 1); r.lp.setStep(1, 0, 2); r.lp.setSong(song);
        r.silence(3 * 24000 + 64);
        if (song) CHECK(r.lp.part() == 0 && r.lp.songStep() == 1 && r.lp.songPass() == 1);
        return slice(r.out, r.out.size() - (3 * 24000 + 64), r.out.size());
    };
    CHECK(maxAbsDiff(run(true), run(false)) < 1e-6f);
}

// B's double-tap: newPart(t) goes to the next part; an empty one gets the tracks below t copied, t recording from the
// switch sample, and nothing above.
static void newPartCopiesBelowAndRecordsTheSelectedTrack() {
    Rig r; recordBase(r, 24000, 0.1f); recordBaseOn(r, 1); recordBaseOn(r, 2, 0.1f, 660.0);
    r.silence(6000); r.lp.newPart(2);
    CHECK(r.lp.queuedPart() == 1 && r.lp.newPartTrack() == 2 && r.lp.part() == 0);
    r.silence(24000 - r.lp.position() - 1); CHECK(r.lp.part() == 0);
    r.silence(2);
    CHECK(r.lp.part() == 1 && r.lp.state(0) == TrackState::Playing && r.lp.state(1) == TrackState::Playing);
    CHECK(r.lp.state(2) == TrackState::Recording && r.lp.state(3) == TrackState::Empty && r.lp.newPartTrack() == -1);
    r.silence(24000 - 1); r.lp.pressRec(2);                               // B: one loop since the switch
    CHECK(r.lp.trackLength(2) == 24000);
}
// Review Focus 2: track 2 records from part B's first sample. 0.5 on B's first 2000 samples, silence around: track 2
// holds it under its kFadeInMs fade-in from its beginning, which pins both the start sample (a start back-dated or
// late shifts or flattens the ramp) and the fade-in (every recording fades in, so the first sample is recorded at 0).
static void newPartRecordsFromTheSwitchSample() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.newPart(2); r.silence(24000 - r.lp.position());   // up to the switch
    CHECK(r.lp.part() == 1 && r.lp.state(2) == TrackState::Recording);
    std::vector<float> in(24000, 0.0f);
    std::fill(in.begin(), in.begin() + 2000, 0.5f);
    r.feed(in); r.lp.pressRec(2); r.silence(4800);                       // closed at one loop, its punch-out done
    CHECK(r.lp.trackLength(2) == 24000);
    ouro::PieceMeta m; CHECK(r.lp.takeSnapshot(m));
    std::vector<float> t2(24000); r.lp.readSnapshot(1, 2, 0, t2.data(), 24000);   // track 2 alone, from its beginning
    r.lp.dropSnapshot();
    float e = 0.0f;
    for (uint32_t i = 0; i < 24000; ++i) e = std::max(e, std::fabs(t2[i] - (i < 2000 ? 0.5f * fadeInGain(i) : 0.0f)));
    CHECK(e < 1e-6f);
}
// Beyond the brief: an overdub running at the switch closes there, and the new recording takes exactly the rest of the
// input under its punch-out fade (which track 0's copy keeps), as a one-press handover does: no dip, no doubling.
static void newPartTakesTheInputOverAClosingTake() {
    Rig r; r.lp.setTailMs(0); recordBase(r, 24000, 0.0f);               // a silent loop
    r.silence(6000); r.lp.pressRec(0); r.lp.newPart(2);                 // overdubbing at A's end
    r.feed(std::vector<float>(24000 - r.lp.position() + 24000, 0.5f)); r.lp.pressRec(2); r.silence(4800);
    CHECK(r.lp.part() == 1 && r.lp.trackLength(2) == 24000);
    ouro::PieceMeta m; CHECK(r.lp.takeSnapshot(m));
    std::vector<float> t0(480), t2(480);                                 // from B's first sample: the fade and past it
    r.lp.readSnapshot(1, 0, 0, t0.data(), 480); r.lp.readSnapshot(1, 2, 0, t2.data(), 480);
    r.lp.dropSnapshot();
    float e = 0.0f;
    for (uint32_t i = 0; i < 480; ++i) e = std::max(e, std::fabs(t0[i] + t2[i] - 0.5f));
    CHECK(e < 1e-6f && t0[100] > 0.1f && t2[100] > 0.1f);                // both share the fade's middle
}
static void newPartAboveTheSelectedTrackIsSilent() {
    Rig r; const auto a = recordBase(r, 24000, 0.1f);                    // track 0 alone
    recordBaseOn(r, 1, 0.4f, 471.0); recordBaseOn(r, 2, 0.2f, 660.0);   // loud: track 1, and one above it
    r.lp.newPart(1); r.silence(24000 - r.lp.position());                 // the switch
    r.silence(ouro::msToSamples(ouro::kFadeInMs, 48000.0));              // its fade-in
    const uint32_t p0 = r.lp.position();
    const auto o = r.capture(24000);
    std::vector<float> want(24000);
    for (uint32_t i = 0; i < 24000; ++i) want[i] = a[(p0 + i) % 24000];
    CHECK(maxAbsDiff(o, want) < 1e-6f);
}
static void newPartTakesOnAndVolumeFromThePartLeft() {
    Rig r; recordBase(r, 24000, 0.1f); recordBaseOn(r, 2);
    r.lp.setVolumeDb(0, -6.0f); r.lp.setOn(1, false); r.lp.setVolumeDb(1, -12.0f);   // track 1 empty
    r.lp.setOn(2, false); r.lp.setVolumeDb(2, -3.0f);
    r.lp.newPart(2); r.silence(24000 - r.lp.position());
    CHECK(r.lp.part() == 1 && r.lp.volumeDb(0) == -6.0f && !r.lp.isOn(1) && r.lp.volumeDb(1) == -12.0f);
    CHECK(r.lp.isOn(2) && r.lp.volumeDb(2) == -3.0f);                    // forced on: its recording is heard
}
static void newPartToAPartWithAudioIsAPlainSwitch() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.setPart(1); r.silence(24000);
    r.lp.pressRec(0); r.feed(sine(330, 0.05f, 24000)); r.lp.pressRec(0); r.silence(4800);   // B: 2 layers on track 0
    const uint32_t layers = r.lp.layerCount(0), len = r.lp.trackLength(0);
    r.lp.setPart(0); r.silence(24000); CHECK(r.lp.part() == 0);
    r.lp.newPart(2); CHECK(r.lp.newPartTrack() == -1 && r.lp.queuedPart() == 1);
    r.silence(24000);
    CHECK(r.lp.part() == 1 && r.lp.layerCount(0) == layers && r.lp.trackLength(0) == len);
    for (int t = 1; t < 4; ++t) CHECK(r.lp.state(t) == TrackState::Empty);
    CHECK(r.lp.state(0) == TrackState::Playing);
}
static void newPartWrapsFromDToA() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.setPart(3); r.silence(24000);
    CHECK(r.lp.part() == 3); r.lp.newPart(1); CHECK(r.lp.queuedPart() == 0);
}
static void aTapOnBCancelsTheNewPart() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.newPart(2); r.lp.pressRec(0);
    CHECK(r.lp.queuedPart() == -1 && r.lp.newPartTrack() == -1 && r.lp.state(0) == TrackState::Playing);
}
static void cancelWaitCancelsTheNewPartOnAnyTrack() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.newPart(2);
    CHECK(r.lp.cancelWait(0) && r.lp.newPartTrack() == -1 && r.lp.queuedPart() == -1);
}
// Review I2: the first loop's close waiting for its line (Sync) and a new part waiting: cancelWait() (C's first press)
// cancels only the new part, as a press on B does; the close stays and lands on the line.
static void cancelWaitKeepsAWaitingClose() {
    SyncRig r; r.lp.setSync(true); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 2 * 96000 - 48000)); r.lp.pressRec(0);   // half a bar early: waits
    r.lp.newPart(2);
    CHECK(r.lp.userWaiting(0) && r.lp.queuedPart() == 1 && r.lp.newPartTrack() == 2);
    CHECK(r.lp.cancelWait(0) && r.lp.newPartTrack() == -1 && r.lp.queuedPart() == -1 && r.lp.userWaiting(0));
    r.silenceSync(48000 + 128);
    CHECK(!r.lp.waiting() && r.lp.state(0) == TrackState::Playing && r.lp.loopLength() == 2 * 96000);
}
static void partKnobStopAndClearAllDropTheNewPart() {
    Rig r; recordBase(r, 24000, 0.1f);
    r.lp.newPart(2); r.lp.setPart(3); CHECK(r.lp.queuedPart() == 3 && r.lp.newPartTrack() == -1);
    r.lp.newPart(2); r.lp.setPart(0); CHECK(r.lp.queuedPart() == -1 && r.lp.newPartTrack() == -1);
    r.lp.newPart(2); r.lp.stop(ouro::StopMode::Instant); CHECK(r.lp.newPartTrack() == -1);
    r.silence(2000); CHECK(r.lp.transport() == ouro::Transport::Stopped && r.lp.part() == 0);
    r.lp.restart(); r.silence(2000);                                     // running again
    r.lp.newPart(2); r.lp.clearAll(); CHECK(r.lp.newPartTrack() == -1);
}
static void newPartTurnsTheSongOff() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.setSong(true); r.lp.newPart(1); CHECK(!r.lp.songOn());
}
// Beyond the brief: Song turned on drops a queued switch, and a waiting new part with it.
static void songOnDropsTheNewPart() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.newPart(2); r.lp.setSong(true);
    CHECK(r.lp.queuedPart() == -1 && r.lp.newPartTrack() == -1 && !r.lp.recWaiting());
}
static void newPartArmsWithAutoRec() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.setAutoRec(true); r.lp.newPart(1);
    r.silence(24000 - r.lp.position());
    CHECK(r.lp.part() == 1 && r.lp.armedTrack() == 1 && r.lp.state(1) == TrackState::Empty);
}
// Review Focus 3: a long copy below makes the new part's pass; the new recording, closed longer, makes it then.
static void newPartPassFollowsALongCopy() {
    const uint32_t L = 24000;
    Rig r; r.lp.pressRec(2); r.feed(sine(220, 0.1f, L)); r.lp.pressRec(2);       // the first loop, on track 2
    r.lp.pressRec(0); r.feed(sine(330, 0.1f, 2 * L)); r.lp.pressRec(0);         // track 0: two loops
    CHECK(r.lp.trackLength(0) == 2 * L);
    r.lp.newPart(1); r.silence(2 * L - r.lp.trackPosition(0));
    CHECK(r.lp.part() == 1 && r.lp.partLength() == 2 * L);
    r.feed(sine(440, 0.1f, 13 * L / 5)); r.lp.pressRec(1);                      // 2.6 loops
    CHECK(r.lp.trackLength(1) == 3 * L && r.lp.partLength() == 3 * L);
}
// Review Focus 4: the pool fills during the new part's recording: it closes as at the limit, the copy still plays.
static void newPartWithMemoryFullStillCopies() {
    Rig r(48000.0, 128, size_t(1) << 20);                               // 64 blocks: about 5.5 s of audio
    recordBase(r, 24000, 0.1f); r.lp.newPart(1); r.silence(24000 - r.lp.position());
    CHECK(r.lp.part() == 1 && r.lp.state(1) == TrackState::Recording);
    r.feed(sine(330, 0.1f, 10 * 48000));                                 // track 1 until the pool is full
    CHECK(r.lp.memoryFull() && r.lp.state(1) == TrackState::Playing && r.lp.state(0) == TrackState::Playing);
}
static void recWaitingWhileANewPartWaits() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.newPart(1);
    CHECK(r.lp.recWaiting() && !r.lp.userWaiting());
}
// Beyond the brief: queued during the first recording, the new part waits for the part end after its close; that
// recording aborted, nothing is recorded and the switch happens at once, a plain one (no loop to record onto).
static void aFirstRecordingAbortedUnderANewPartSwitchesPlainly() {
    Rig r; r.lp.pressRec(0); r.feed(sine(220, 0.1f, 12000)); r.lp.newPart(2);
    CHECK(r.lp.part() == 0 && r.lp.queuedPart() == 1 && r.lp.newPartTrack() == 2);
    r.lp.undo(0);
    CHECK(r.lp.part() == 1 && r.lp.queuedPart() == -1 && r.lp.newPartTrack() == -1);
    CHECK(r.lp.state(2) == TrackState::Empty && !r.lp.recWaiting());
}

// Stopped, the new part is switched to at once (copies below, nothing above) and starts with t recording.
static void newPartWhileStoppedSwitchesAndStartsRecording() {
    Rig r; recordBase(r, 24000, 0.1f); recordBaseOn(r, 2);              // track 2: above the new part's track
    r.lp.stop(ouro::StopMode::Instant); r.silence(2000);
    CHECK(r.lp.transport() == ouro::Transport::Stopped);
    r.lp.newPart(1);
    CHECK(r.lp.part() == 1 && r.lp.transport() == ouro::Transport::Running && r.lp.state(0) == TrackState::Playing);
    CHECK(r.lp.state(1) == TrackState::Recording && r.lp.state(2) == TrackState::Empty && r.lp.newPartTrack() == -1);
    r.silence(24000); r.lp.pressRec(1); CHECK(r.lp.trackLength(1) == 24000);   // from the start, the loop's top
}
// Stopping (the plugin stopped Instant first): the park switches and restarts, t recording.
static void newPartWhileStoppingStartsAtThePark() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.stop(ouro::StopMode::Instant); r.lp.newPart(1);
    CHECK(r.lp.transport() == ouro::Transport::Stopping && r.lp.part() == 0 && r.lp.newPartTrack() == 1);
    r.silence(2000);
    CHECK(r.lp.part() == 1 && r.lp.transport() == ouro::Transport::Running && r.lp.state(1) == TrackState::Recording);
    CHECK(r.lp.state(0) == TrackState::Playing && r.lp.newPartTrack() == -1);
    Rig q; recordBase(q, 24000, 0.1f); q.lp.stop(ouro::StopMode::Instant); q.lp.newPart(1);
    q.lp.pressRec(0); q.silence(2000);                                   // beyond the brief: B cancels the restart too
    CHECK(q.lp.part() == 0 && q.lp.transport() == ouro::Transport::Stopped && q.lp.newPartTrack() == -1);
}
// A 1-bar loop on MOD's grid, stopped half a bar before MOD's next line (R1: MOD plays, so a start waits for it).
static void stopMidBar(SyncRig& r) {
    r.lp.setSync(true); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 96000)); r.lp.pressRec(0); r.silenceSync(20000);
    r.lp.stop(ouro::StopMode::Instant); r.silenceSync(2000); r.toPhase(48000);
    CHECK(r.lp.transport() == ouro::Transport::Stopped);
}
// R1: stopped while MOD plays, the start waits for the line; a press before it cancels it, the switch stays.
static void newPartWhileStoppedWithSyncWaitsForTheLine() {
    SyncRig r; stopMidBar(r); r.lp.newPart(1);
    CHECK(r.lp.part() == 1 && r.lp.transport() == ouro::Transport::Stopped && r.lp.newPartTrack() == 1);
    CHECK(r.lp.recWaiting() && r.lp.state(0) == TrackState::Playing);
    r.silenceSync(48000 - 128); CHECK(r.lp.transport() == ouro::Transport::Stopped);
    r.silenceSync(256);
    CHECK(r.lp.transport() == ouro::Transport::Running && r.lp.state(1) == TrackState::Recording);
    CHECK(r.lp.newPartTrack() == -1);
    SyncRig q; stopMidBar(q); q.lp.newPart(1); q.lp.pressRec(0);
    CHECK(q.lp.newPartTrack() == -1 && q.lp.part() == 1 && q.lp.transport() == ouro::Transport::Stopped);
    CHECK(q.lp.state(1) == TrackState::Empty && !q.lp.recWaiting());
    q.silenceSync(48000 + 128);                                          // past the line: still stopped
    CHECK(q.lp.transport() == ouro::Transport::Stopped && q.lp.state(1) == TrackState::Empty);
}
// Beyond the brief: stopped, a restart already waiting (B's first press) is kept for the new part; Stop/Play while it
// waits cancels the start, and the next start records nothing.
static void aStoppedNewPartKeepsAWaitingRestartAndStopPlayDropsIt() {
    SyncRig r; stopMidBar(r); r.lp.restart(); CHECK(r.lp.userWaiting());
    r.lp.newPart(1);
    CHECK(r.lp.part() == 1 && r.lp.userWaiting() && r.lp.newPartTrack() == 1);
    r.lp.restart();                                                      // Stop/Play while stopped: the wait goes
    CHECK(!r.lp.userWaiting() && r.lp.newPartTrack() == -1 && !r.lp.recWaiting());
    r.lp.restart(); r.silenceSync(48000 + 128);
    CHECK(r.lp.transport() == ouro::Transport::Running && r.lp.part() == 1 && r.lp.state(1) == TrackState::Empty);
}
// Beyond the brief: C's cancel, a stop (MOD's, with Sync) and Clear All forget a stopped new part's waiting start.
static void cancelWaitStopAndClearAllDropAStoppedNewPart() {
    { SyncRig r; stopMidBar(r); r.lp.newPart(1);
      CHECK(r.lp.cancelWait(3) && r.lp.newPartTrack() == -1 && !r.lp.userWaiting() && r.lp.part() == 1); }
    { SyncRig r; stopMidBar(r); r.lp.newPart(1); r.lp.stop(ouro::StopMode::Instant);
      CHECK(r.lp.newPartTrack() == -1 && !r.lp.userWaiting());
      r.lp.restart(); r.silenceSync(48000 + 128);                        // the next start records nothing
      CHECK(r.lp.transport() == ouro::Transport::Running && r.lp.state(1) == TrackState::Empty); }
    { SyncRig r; stopMidBar(r); r.lp.newPart(1); r.lp.clearAll(); CHECK(r.lp.newPartTrack() == -1); }
}
// Beyond the brief: a start late within the grace back-dates the new recording to the line, as a press there is.
static void newPartStartedLateInTheGraceKeepsFromTheLine() {
    SyncRig r; r.lp.setSync(true); r.lp.setTailMs(0); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(std::vector<float>(96000, 0.0f)); r.lp.pressRec(0);   // a silent 1-bar loop
    r.silenceSync(20000); r.lp.stop(ouro::StopMode::Instant); r.silenceSync(2000);
    r.toPhase(96000 - 2000); r.feedSync(std::vector<float>(3280, 0.5f));             // 0.5 up to 1280 past the line
    r.lp.newPart(1);
    CHECK(r.lp.transport() == ouro::Transport::Running && r.lp.position() == 1280);
    CHECK(r.lp.state(1) == TrackState::Recording);
    r.silenceSync(96000 - 1280); r.lp.pressRec(1); r.silenceSync(4800);             // closed at one loop
    CHECK(r.lp.trackLength(1) == 96000);
    ouro::PieceMeta m; CHECK(r.lp.takeSnapshot(m));
    std::vector<float> t1(2000); r.lp.readSnapshot(1, 1, 0, t1.data(), 2000);         // from its beginning: the line
    r.lp.dropSnapshot();
    float e = 0.0f;
    for (uint32_t i = 0; i < 2000; ++i) e = std::max(e, std::fabs(t1[i] - (i < 1280 ? 0.5f * fadeInGain(i) : 0.0f)));
    CHECK(e < 1e-6f);
}
static void newPartWhileStoppedArmsWithAutoRec() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.stop(ouro::StopMode::Instant); r.silence(2000);
    r.lp.setAutoRec(true); r.lp.newPart(1);
    CHECK(r.lp.part() == 1 && r.lp.transport() == ouro::Transport::Running && r.lp.armedTrack() == 1);
    CHECK(r.lp.state(1) == TrackState::Empty);
}
static void newPartWithNothingRecordedMovesAndRecords() {
    Rig r; r.lp.newPart(0);
    CHECK(r.lp.part() == 1 && r.lp.state(0) == TrackState::Recording && r.lp.loopLength() == 0);
}
// dropStart: the second press keeps a waiting close of the first recording (never Rec->Dub) and drops any start.
static void dropStartKeepsAWaitingCloseOfTheFirstRecording() {
    SyncRig r; r.lp.setSync(true); r.lp.setRecToDub(true); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 2 * 96000 - 48000)); r.lp.pressRec(0);   // half a bar early: waits
    CHECK(r.lp.userWaiting(0));
    r.lp.dropStart(0); CHECK(r.lp.userWaiting(0) && r.lp.recWaiting());
    r.silenceSync(48000 + 128);
    CHECK(!r.lp.waiting() && r.lp.loopLength() == 2 * 96000 && r.lp.state(0) == TrackState::Playing);
}
static void dropStartTurnsAWaitingHandoverIntoAClose() {
    SyncRig r; r.lp.setSync(true); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 2 * 96000 - 48000)); r.lp.pressRec(1);   // a handover: waits
    CHECK(r.lp.userWaiting(1));
    r.lp.dropStart(1); CHECK(r.lp.userWaiting(0) && !r.lp.userWaiting(1));             // the first recording's close
    r.silenceSync(48000 + 128);
    CHECK(r.lp.loopLength() == 2 * 96000 && r.lp.state(0) == TrackState::Playing && r.lp.state(1) == TrackState::Empty);
}
static void dropStartCancelsAWaitingStartAndAnArm() {
    SyncRig r; r.lp.setSync(true); r.lp.setTailMs(0); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(std::vector<float>(96000, 0.0f)); r.lp.pressRec(0);   // a silent 1-bar loop
    r.toPhase(48000); r.lp.pressRec(1); CHECK(r.lp.userWaiting(1));
    r.lp.dropStart(1); CHECK(!r.lp.userWaiting());
    r.silenceSync(48000 + 128); CHECK(r.lp.state(1) == TrackState::Empty);
    r.lp.setAutoRec(true); r.lp.pressRec(2); CHECK(r.lp.armedTrack() == 2);
    r.lp.dropStart(2); CHECK(r.lp.armedTrack() == -1);
    r.lp.pressRec(2); r.toPhase(7680); r.feedSync(sine(220, 0.2f, 128));   // the arm fires past the grace: waits
    CHECK(r.lp.userWaiting(2));
    r.lp.dropStart(2); CHECK(!r.lp.userWaiting());
    r.silenceSync(96000); CHECK(r.lp.state(2) == TrackState::Empty);
}
// Review Focus 6: the close waits for its line, the new part for the end of the first pass that close makes.
static void newPartAfterAWaitingCloseSwitchesAtThatPassEnd() {
    SyncRig r; r.lp.setSync(true); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 2 * 96000 - 48000)); r.lp.pressRec(0);   // half a bar early: waits
    r.lp.dropStart(0); r.lp.newPart(1);
    CHECK(r.lp.userWaiting(0) && r.lp.queuedPart() == 1 && r.lp.newPartTrack() == 1);
    r.silenceSync(48000 + 128);                                          // the line: the loop closes
    CHECK(r.lp.loopLength() == 2 * 96000 && r.lp.state(0) == TrackState::Playing && r.lp.part() == 0);
    if (r.lp.loopLength() != 2 * 96000) return;                          // not closed: the count below would wrap
    r.silenceSync(r.lp.loopLength() - r.lp.position() - 1); CHECK(r.lp.part() == 0);
    r.silenceSync(2);
    CHECK(r.lp.part() == 1 && r.lp.state(0) == TrackState::Playing && r.lp.state(1) == TrackState::Recording);
}

int main() {
    switchWaitsForThePartEnd(); copyIsIndependentAndKeepsOnAndVolume(); copyOnEntryAllocatesNothing();
    partLengthIsTheLongestTrack(); choosingTheCurrentPartCancels(); stoppedSwitchesAtOnce(); firstRecordingDefersTheSwitch();
    aRunningTakeClosesAndTheCopyHasItsPunchOut(); noClickAtTheSwitch(); undoOnACopiedTrack(); clearedPartRefillsOnEntry();
    lResetsOnlyWhenEveryPartIsEmpty(); clearAllEmptiesEveryPartAndGoesToA(); anEmptyPartLastsOneLoop();
    modStartKeepsAQueuedSwitch(); aPendingUndoReachesTheCopy(); theSwitchIntoACopyIsSeamless();
    aPartLeftWhileStoppedLetsLReset(); aFirstRecordingEndingWithoutALoopSwitchesAtOnce();
    clearAllInsideTheSwitchCrossfadeDoesNotClick(); theSwitchCrossfadesOverTheFadeIn();
    aSongStepOnTheCurrentPartIsNoSwitch();
    newPartCopiesBelowAndRecordsTheSelectedTrack(); newPartRecordsFromTheSwitchSample();
    newPartTakesTheInputOverAClosingTake(); newPartAboveTheSelectedTrackIsSilent();
    newPartTakesOnAndVolumeFromThePartLeft();
    newPartToAPartWithAudioIsAPlainSwitch(); newPartWrapsFromDToA(); aTapOnBCancelsTheNewPart();
    cancelWaitCancelsTheNewPartOnAnyTrack(); cancelWaitKeepsAWaitingClose(); partKnobStopAndClearAllDropTheNewPart();
    newPartTurnsTheSongOff();
    songOnDropsTheNewPart(); newPartArmsWithAutoRec(); newPartPassFollowsALongCopy(); newPartWithMemoryFullStillCopies();
    recWaitingWhileANewPartWaits(); aFirstRecordingAbortedUnderANewPartSwitchesPlainly();
    newPartWhileStoppedSwitchesAndStartsRecording(); newPartWhileStoppingStartsAtThePark();
    newPartWhileStoppedWithSyncWaitsForTheLine(); newPartWithNothingRecordedMovesAndRecords();
    dropStartKeepsAWaitingCloseOfTheFirstRecording(); dropStartTurnsAWaitingHandoverIntoAClose();
    dropStartCancelsAWaitingStartAndAnArm(); newPartAfterAWaitingCloseSwitchesAtThatPassEnd();
    aStoppedNewPartKeepsAWaitingRestartAndStopPlayDropsIt(); cancelWaitStopAndClearAllDropAStoppedNewPart();
    newPartStartedLateInTheGraceKeepsFromTheLine();
    newPartWhileStoppedArmsWithAutoRec();
    REPORT();
}
