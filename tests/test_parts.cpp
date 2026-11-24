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

int main() {
    switchWaitsForThePartEnd(); copyIsIndependentAndKeepsOnAndVolume(); copyOnEntryAllocatesNothing();
    partLengthIsTheLongestTrack(); choosingTheCurrentPartCancels(); stoppedSwitchesAtOnce(); firstRecordingDefersTheSwitch();
    aRunningTakeClosesAndTheCopyHasItsPunchOut(); noClickAtTheSwitch(); undoOnACopiedTrack(); clearedPartRefillsOnEntry();
    lResetsOnlyWhenEveryPartIsEmpty(); clearAllEmptiesEveryPartAndGoesToA(); anEmptyPartLastsOneLoop();
    modStartKeepsAQueuedSwitch(); aPendingUndoReachesTheCopy(); theSwitchIntoACopyIsSeamless();
    aPartLeftWhileStoppedLetsLReset(); aFirstRecordingEndingWithoutALoopSwitchesAtOnce();
    clearAllInsideTheSwitchCrossfadeDoesNotClick(); theSwitchCrossfadesOverTheFadeIn();
    aSongStepOnTheCurrentPartIsNoSwitch();
    REPORT();
}
