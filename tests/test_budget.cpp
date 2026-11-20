#include "looper_rig.hpp"

// Overdub n samples of sine(330) on track t, 5 ms punch-out included.
static void dub(Rig& r, int t, uint32_t n) { r.lp.pressRec(t); r.feed(sine(330, 0.1f, n)); r.lp.pressRec(t); r.settle(); }

static void fullLayersEvictOldest() {
    Rig r; r.lp.setTailMs(0); r.lp.setBudgetMb(0.25f); recordBase(r, 48000, 0.1f);
    dub(r, 0, 48000); CHECK(r.lp.undoDepth(0) == 1);
    dub(r, 0, 48000); CHECK(r.lp.undoDepth(0) == 1);
    CHECK(r.lp.undoBytes() <= 262144);
}
static void newDubDiscardsOwnRedoBeforeEvicting() {
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

// Evicting an older layer of the writing track while a new overdub is stored must keep that delta intact.
static void evictionKeepsTheLiveDeltaIntact() {
    Rig r; r.lp.setTailMs(0); r.lp.setBudgetMb(0.25f); recordBase(r, 48000, 0.1f);
    dub(r, 0, 48000); auto withA = r.settle();                     // A: 12 of 16 blocks
    dub(r, 0, 48000); CHECK(r.lp.undoDepth(0) == 1);               // B evicts A mid-write (merged into the buffer)
    r.lp.undo(0); CHECK(maxAbsDiff(r.settle(), withA) < 1e-5f);    // undoing B leaves base + A
    r.lp.reset(); CHECK(r.lp.undoBytes() == 0);                    // no leaked block
}

// A punch-in's back-dated D samples can need a block: evicting this track's own layer then must keep the new
// overdub's slot in the stack (no leaked block, undo still exact).
static void punchInEvictsOwnLayer() {
    Rig r; r.lp.setTailMs(0); r.lp.setBudgetMb(0.0625f); recordBase(r, 48000, 0.1f);   // 4 blocks
    r.lp.setTriggerLatencyMs(3.0f);                                // D = 144
    dub(r, 0, 12000); auto withA = r.loopImage();                  // A: 4 blocks, the pool is full
    dub(r, 0, 3900); CHECK(r.lp.undoDepth(0) == 1);                // B's back-dated start merges A
    r.lp.undo(0); CHECK(maxAbsDiff(r.settle(), withA) < 1e-5f);    // undoing B leaves base + A
    r.lp.reset(); CHECK(r.lp.undoBytes() == 0);                    // no leaked block
}
static void globallyOldestMerged() {
    Rig r; r.lp.setTailMs(0); r.lp.setBudgetMb(0.25f); recordBase(r, 48000, 0.1f);
    recordBaseOn(r, 1); dub(r, 0, 14400); dub(r, 1, 14400);          // A(t0), B(t1): 4 + 4 blocks
    dub(r, 0, 48000);                                              // C needs 12: A merged
    CHECK(r.lp.undoDepth(0) == 1 && r.lp.undoDepth(1) == 1);
    dub(r, 1, 48000);                                              // D needs 12: B, then C merged
    CHECK(r.lp.undoDepth(0) == 0 && r.lp.undoDepth(1) == 1);
}
// R11: a redo entry on another track goes before this track's older undo layer.
static void redoDroppedBeforeUndo() {
    Rig r; r.lp.setTailMs(0); r.lp.setBudgetMb(0.25f); recordBase(r, 48000, 0.1f);
    recordBaseOn(r, 1); dub(r, 0, 14400);                          // A(t0): 4 blocks, the oldest layer
    dub(r, 1, 14400); r.lp.undo(1); r.settle();                    // B(t1): 4 blocks, now a redo entry
    CHECK(r.lp.canRedo(1));
    dub(r, 0, 48000);                                              // C needs 12: dropping B alone makes room
    CHECK(!r.lp.canRedo(1) && r.lp.undoDepth(0) == 2);             // A kept
}
// The next redo is the layer undone last: a redo eviction drops the entry undone longest ago (redone last).
static void redoEvictionKeepsTheNextRedo() {
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.0f); recordBaseOn(r, 1, 0.0f);   // silent bases
    r.lp.setOn(0, false); r.settle();                              // the image is t1 alone
    r.lp.pressRec(1); r.feed(sine(1000, 0.1f, 24064)); r.lp.pressRec(1); auto withL2 = r.settle();   // L2: 6 blocks
    r.lp.pressRec(1); r.feed(sine(2000, 0.1f, 24064)); r.lp.pressRec(1); r.settle();                  // L3: 6 blocks
    r.lp.undo(1); r.settle(); r.lp.undo(1); r.settle();            // redo stack: L2 (next), then L3
    r.lp.setBudgetMb(float(r.lp.undoBytes() + 16384) / 1048576.0f);   // one free block
    dub(r, 0, 2 * 4096 + 100);                                     // 3 blocks: one redo entry goes
    CHECK(r.lp.canRedo(1));
    r.lp.redo(1); CHECK(!r.lp.canRedo(1) && maxAbsDiff(r.settle(), withL2) < 1e-5f);
}

int main() {
    evictionKeepsTheLiveDeltaIntact(); fullLayersEvictOldest(); newDubDiscardsOwnRedoBeforeEvicting(); oversizedOverdubKeepsRecording();
    punchInEvictsOwnLayer(); globallyOldestMerged(); redoDroppedBeforeUndo(); redoEvictionKeepsTheNextRedo();
    REPORT();
}
