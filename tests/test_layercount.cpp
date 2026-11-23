#include "looper_rig.hpp"
using ouro::TrackState;

// layerCount(t): the layers heard now on track t. 0 empty; else the recording plus the dubs applied (one in progress too).
static uint32_t n(Rig& r, int t = 0) { return r.lp.layerCount(t); }
static void dub(Rig& r, int t, uint32_t len) { r.lp.pressRec(t); r.feed(sine(330, 0.1f, len)); r.lp.pressRec(t); r.settle(); }

static void recordDubUndoRedoClear() {
    Rig r; r.lp.setTailMs(0);
    CHECK(n(r) == 0);
    r.lp.pressRec(0); r.feed(sine(220, 0.1f, 24064)); CHECK(n(r) == 1);   // the first recording
    r.lp.pressRec(0); r.settle(); CHECK(n(r) == 1);
    r.lp.pressRec(0); r.feed(sine(330, 0.1f, 9600)); CHECK(n(r) == 2);    // during the dub
    r.lp.pressRec(0); r.settle(); CHECK(n(r) == 2);                       // and after it
    r.lp.undo(0); CHECK(n(r) == 1);
    r.lp.redo(0); CHECK(n(r) == 2);
    r.lp.undo(0); r.lp.undo(0);                                           // the recording undone: empty
    CHECK(r.lp.state(0) == TrackState::Empty && r.lp.canRedo(0) && n(r) == 0);
    r.lp.redo(0); CHECK(n(r) == 1);
    r.lp.redo(0); CHECK(n(r) == 2);
    r.lp.clearTrack(0); CHECK(n(r) == 0);
    CHECK(n(r, 1) == 0 && r.lp.layerCount(-1) == 0 && r.lp.layerCount(4) == 0);
}
static void cancelledAndSilentDubsDontCount() {
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.1f);
    r.lp.pressRec(0); r.feed(sine(330, 0.1f, 4800)); CHECK(n(r) == 2);
    r.lp.undo(0); CHECK(n(r) == 1); r.settle(); CHECK(n(r) == 1);         // cancelled
    r.lp.pressRec(0); r.feed(std::vector<float>(9600, 1e-5f)); CHECK(n(r) == 2);
    r.lp.pressRec(0); r.settle(); CHECK(n(r) == 1 && r.lp.undoDepth(0) == 0);   // silent: subtracted, no layer
}
static void recordingAbortedIsEmpty() {
    Rig r; r.lp.setTailMs(0);
    r.lp.pressRec(0); r.feed(sine(220, 0.1f, 9600)); r.lp.undo(0); CHECK(n(r) == 0);
    recordBase(r, 24064, 0.1f);
    r.lp.pressRec(1); r.feed(sine(330, 0.1f, 4800)); CHECK(n(r, 1) == 1);  // onto the loop
    r.lp.undo(1); CHECK(n(r, 1) == 0 && n(r) == 1);
}
static void recToDubCountsTheDub() {
    Rig r; r.lp.setTailMs(0); r.lp.setRecToDub(true);
    r.lp.pressRec(0); r.feed(sine(220, 0.1f, 24064)); r.lp.pressRec(0);
    CHECK(r.lp.state(0) == TrackState::Overdubbing && n(r) == 2);
    r.feed(sine(220, 0.1f, 9600)); r.lp.pressRec(0); r.settle(); CHECK(n(r) == 2);
    r.lp.pressRec(1); r.feed(sine(330, 0.1f, 24064)); CHECK(n(r, 1) == 1);   // a recording onto the loop, Rec->Dub
    r.lp.pressRec(1); CHECK(r.lp.state(1) == TrackState::Overdubbing && n(r, 1) == 2);
    r.feed(sine(330, 0.1f, 9600)); r.lp.pressRec(1); r.settle(); CHECK(n(r, 1) == 2);
    r.lp.undo(1); CHECK(n(r, 1) == 1); r.lp.undo(1); CHECK(n(r, 1) == 0);
}
static void longTracksCountLikeOneLoop() {
    for (uint32_t len : { 38400u, 52800u }) {                             // 1.6 loops (rounds up), 2.2 (rounds down, folds)
        Rig r; r.lp.setTailMs(0); recordBase(r, 24000, 0.0f);
        r.lp.pressRec(1); r.feed(sine(220, 0.1f, len)); CHECK(n(r, 1) == 1);
        r.lp.pressRec(1); r.settle(); CHECK(r.lp.trackLength(1) == 48000 && n(r, 1) == 1);
        r.lp.pressRec(1); r.feed(sine(330, 0.1f, 60000)); CHECK(n(r, 1) == 2);   // a dub over more than one pass
        r.lp.pressRec(1); r.silence(48000); CHECK(n(r, 1) == 2 && r.lp.undoDepth(1) == 1);
        r.lp.undo(1); CHECK(n(r, 1) == 1);
    }
}
// An evicted undo layer stays in the loop (baked into the base): the count keeps it, and so does a dub the full pool
// could not store. Undo then reaches the base sooner: past the remaining undo layers it empties the track.
static void evictionKeepsTheCount() {
    Rig r; r.lp.setTailMs(0); r.lp.setBudgetMb(0.25f); recordBase(r, 48000, 0.1f);
    dub(r, 0, 48000); CHECK(n(r) == 2);
    dub(r, 0, 48000); CHECK(r.lp.undoDepth(0) == 1 && n(r) == 3);      // the first dub was evicted
    r.lp.undo(0); CHECK(n(r) == 2);
    r.lp.redo(0); CHECK(n(r) == 3);
    r.lp.undo(0); r.lp.undo(0); CHECK(n(r) == 0);                      // the base, with the evicted dub in it
    r.lp.redo(0); CHECK(n(r) == 2); r.lp.redo(0); CHECK(n(r) == 3);
    r.lp.clearTrack(0); CHECK(n(r) == 0);
    r.lp.pressRec(0); CHECK(n(r) == 1);                                // a new recording starts from 1
}
static void unstoredDubStillCounts() {
    Rig r; r.lp.setTailMs(0); r.lp.setBudgetMb(0.05f); recordBase(r, 48000, 0.1f);
    dub(r, 0, 48000); CHECK(r.lp.undoDepth(0) == 0 && n(r) == 2);
}
static void tracksCountApart() {
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.1f); recordBaseOn(r, 1); r.settle();
    dub(r, 1, 9600); dub(r, 1, 9600);
    CHECK(n(r, 0) == 1 && n(r, 1) == 3 && n(r, 2) == 0);
    r.lp.reset(); CHECK(n(r, 0) == 0 && n(r, 1) == 0);
}

int main() {
    recordDubUndoRedoClear(); cancelledAndSilentDubsDontCount(); recordingAbortedIsEmpty(); recToDubCountsTheDub();
    longTracksCountLikeOneLoop(); evictionKeepsTheCount(); unstoredDubStillCounts(); tracksCountApart();
    REPORT();
}
