#include "looper_rig.hpp"

// Overdub n samples of sine(330) on track t, 5 ms punch-out included.
static void dub(Rig& r, int t, uint32_t n) { r.lp.pressRec(t); r.feed(sine(330, 0.1f, n)); r.lp.pressRec(t); r.settle(); }

static void fullLayersEvictOldest() {
    Rig r; r.lp.setTailMs(0); r.lp.setBudgetMb(0.25f); recordBase(r, 48000, 0.1f);
    dub(r, 0, 48000); CHECK(r.lp.undoDepth(0) == 1);
    dub(r, 0, 48000); CHECK(r.lp.undoDepth(0) == 1);
    CHECK(r.lp.undoBytes() <= 262144);
}
static void redoDroppedBeforeUndo() {
    Rig r; r.lp.setTailMs(0); r.lp.setBudgetMb(0.25f); recordBase(r, 48000, 0.1f);
    dub(r, 0, 14400); dub(r, 0, 14400); r.lp.undo(0); r.settle();   // A, B(redo): 4 + 4 blocks
    dub(r, 0, 36000);                                              // C: 9 blocks
    CHECK(r.lp.undoDepth(0) == 2 && !r.lp.canRedo(0));             // A kept, B dropped
}
static void oversizedOverdubKeepsRecording() {
    Rig r; r.lp.setTailMs(0); r.lp.setBudgetMb(0.05f); auto base = recordBase(r, 48000, 0.1f);
    dub(r, 0, 48000);
    CHECK(r.lp.undoDepth(0) == 0 && maxAbsDiff(r.settle(), base) > 0.05f);
}

int main() { fullLayersEvictOldest(); redoDroppedBeforeUndo(); oversizedOverdubKeepsRecording(); REPORT(); }
