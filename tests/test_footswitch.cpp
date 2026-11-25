#include "looper_rig.hpp"

static void dub(Rig& r, uint32_t n) { r.lp.pressRec(0); r.feed(sine(330, 0.1f, n)); r.lp.pressRec(0); r.silence(4800); }

static void peelUndoesTheNewestLayerButNeverTheFirst() {
    Rig r; recordBase(r, 24000, 0.2f); dub(r, 6000); dub(r, 6000);
    CHECK(r.lp.undoDepth(0) == 2);
    r.lp.peel(0); r.silence(1000); CHECK(r.lp.undoDepth(0) == 1);
    r.lp.peel(0); r.silence(1000); CHECK(r.lp.undoDepth(0) == 0 && r.lp.state(0) == ouro::TrackState::Playing);
    r.lp.peel(0); r.silence(1000); CHECK(r.lp.undoDepth(0) == 0 && r.lp.state(0) == ouro::TrackState::Playing);
}
static void peelDropsARunningOverdubThenUndoes() {
    Rig r; recordBase(r, 24000, 0.2f); dub(r, 6000);                    // depth 1
    r.lp.pressRec(0); r.feed(sine(330, 0.1f, 6000));                     // a second overdub running
    r.lp.peel(0); r.silence(1000);
    CHECK(r.lp.state(0) == ouro::TrackState::Playing && r.lp.undoDepth(0) == 0);
}
static void peelLeavesRecordingsAndArmsAlone() {
    Rig r; r.lp.pressRec(0); r.feed(sine(220, 0.1f, 4800));
    r.lp.peel(0); CHECK(r.lp.state(0) == ouro::TrackState::Recording);   // the first recording runs on
    Rig a; a.lp.setAutoRec(true); a.lp.pressRec(0); a.lp.peel(0); CHECK(a.lp.armedTrack() == 0);
}
static void peelCancelsItsTracksPressWait() {
    SyncRig r; r.lp.setSync(true); r.toPhase(96000 - 1280);
    r.lp.pressRec(0); CHECK(r.lp.userWaiting(0));
    r.lp.peel(0); CHECK(!r.lp.userWaiting());
}
static void cancelWaitIsPerTrack() {
    Rig r; recordBase(r, 24000, 0.2f); r.lp.setAutoRec(true); r.lp.pressRec(2);   // track 3 armed
    CHECK(!r.lp.cancelWait(0) && r.lp.armedTrack() == 2);
    CHECK(r.lp.cancelWait(2) && r.lp.armedTrack() == -1);
}
static void closeTakeEndsInPlaying() {
    Rig r; recordBase(r, 24000, 0.2f); dub(r, 6000);
    r.lp.pressRec(0); r.feed(sine(330, 0.1f, 6000));
    r.lp.closeTake(0); CHECK(r.lp.state(0) == ouro::TrackState::Playing && r.lp.undoDepth(0) == 2);
    r.lp.setRecToDub(true); r.lp.pressRec(1); r.feed(sine(440, 0.1f, 30000));   // track 2 records onto the loop
    r.lp.closeTake(1); CHECK(r.lp.state(1) == ouro::TrackState::Playing);      // not Overdubbing
    Rig f; f.lp.pressRec(0); f.feed(sine(220, 0.1f, 4800));
    f.lp.closeTake(0); CHECK(f.lp.state(0) == ouro::TrackState::Recording);    // the first recording: nothing
}

int main() {
    peelUndoesTheNewestLayerButNeverTheFirst(); peelDropsARunningOverdubThenUndoes();
    peelLeavesRecordingsAndArmsAlone(); peelCancelsItsTracksPressWait();
    cancelWaitIsPerTrack(); closeTakeEndsInPlaying();
    REPORT();
}
