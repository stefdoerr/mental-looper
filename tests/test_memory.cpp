#include "looper_rig.hpp"
#include "dsp/TrackBuf.hpp"

using ouro::TrackState;

// TrackBuf: blocks come on the first write, a shared copy costs nothing until written, zero() drops whole blocks.
static void trackBufSharesUntilWritten() {
    const uint32_t K = ouro::BlockPool::kBlock;
    ouro::BlockPool p; CHECK(p.reserve(1 << 20));
    ouro::TrackBuf a, b; CHECK(a.init(3 * K) && b.init(3 * K));
    CHECK(a.get(p, 5000) == 0.0f && p.used() == 0);
    *a.at(p, 5000) = 1.0f; *a.at(p, 9000) = 2.0f;
    CHECK(p.used() == 2 && p.undoUsed() == 0);
    b.shareFrom(p, a); CHECK(p.used() == 2 && b.get(p, 5000) == 1.0f && b.get(p, 9000) == 2.0f);
    *b.at(p, 5000) = 3.0f;                                              // copies its block first
    CHECK(p.used() == 3 && a.get(p, 5000) == 1.0f && b.get(p, 5000) == 3.0f && b.get(p, 5001) == 0.0f);
    a.zero(p, K, 3 * K);                                                // whole blocks: a's own goes, the shared one stays b's
    CHECK(p.used() == 2 && a.get(p, 9000) == 0.0f && b.get(p, 9000) == 2.0f);
    b.zero(p, 9000, 9001); CHECK(b.get(p, 9000) == 0.0f && p.used() == 2);   // part of a block: written
    a.clear(p); b.clear(p); CHECK(p.used() == 0);
}
// Audio blocks ignore the undo budget; a full pool refuses the write until a block comes back.
static void audioIgnoresTheBudgetUntilThePoolIsFull() {
    const uint32_t K = ouro::BlockPool::kBlock;
    ouro::BlockPool p; CHECK(p.reserve(2 * K * sizeof(float))); p.setBudgetBytes(0);
    ouro::TrackBuf a; CHECK(a.init(3 * K));
    CHECK(p.alloc() == -1 && a.at(p, 0) && a.at(p, K) && !p.full());
    CHECK(!a.at(p, 2 * K) && p.full() && a.get(p, 2 * K) == 0.0f);
    a.zero(p, 0, K); CHECK(!p.full() && a.at(p, 2 * K));
    a.clear(p); CHECK(p.used() == 0);
}

static void onlyRecordedAudioUsesThePool() {
    Rig r; const uint32_t before = r.lp.blocksUsed();
    recordBase(r, 48000, 0.1f);
    CHECK(r.lp.blocksUsed() - before <= 48000 / 4096 + 3);              // about one second's blocks, not 5 minutes
    r.lp.clearTrack(0); r.silence(4800);
    CHECK(r.lp.blocksUsed() == before);
}
// A take thrown away (an abort, a clear while it records) gives its blocks back at once: none of it was heard.
static void aThrownAwayTakeGivesItsBlocksBack() {
    Rig r; r.lp.pressRec(0); r.feed(sine(220, 0.1f, 48000)); r.lp.undo(0);
    CHECK(r.lp.blocksUsed() == 0);
    recordBase(r, 48000, 0.1f); const uint32_t base = r.lp.blocksUsed();
    r.lp.pressRec(1); r.feed(sine(330, 0.1f, 48000)); r.lp.clearTrack(1);
    CHECK(base > 0 && r.lp.blocksUsed() == base);
}
static void aFullPoolClosesTheFirstRecording() {
    Rig r(48000.0, 128, size_t(8) << 20);                               // 8 MB: about 43 s of audio
    r.lp.pressRec(0); r.feed(sine(220, 0.1f, 50 * 48000));
    CHECK(r.lp.state(0) == TrackState::Playing && r.lp.memoryFull());
    CHECK(r.lp.loopLength() > 30 * 48000 && r.lp.loopLength() < 44 * 48000);
    r.lp.clearTrack(0); r.silence(4800);
    CHECK(!r.lp.memoryFull());
}
static void aFullPoolClosesARecordingOntoTheLoop() {
    Rig r(48000.0, 128, size_t(8) << 20);
    recordBase(r, 20 * 48000, 0.1f);                                    // about half the pool
    r.lp.pressRec(1); r.feed(sine(330, 0.1f, 40 * 48000));              // a long track until the pool is full
    CHECK(r.lp.memoryFull() && r.lp.state(1) == TrackState::Playing && r.lp.trackLength(1) == 20 * 48000);   // rounded down
}
static void loopsWinOverUndo() {
    Rig r(48000.0, 128, size_t(8) << 20); r.lp.setBudgetMb(16);
    recordBase(r, 10 * 48000, 0.1f);
    for (int k = 0; k < 2; ++k) { r.lp.pressRec(0); r.feed(sine(330, 0.05f, 10 * 48000)); r.lp.pressRec(0); r.silence(4800); }
    const uint32_t depth = r.lp.undoDepth(0);
    r.lp.pressRec(1); r.feed(sine(440, 0.1f, 25 * 48000)); r.lp.pressRec(1);   // needs blocks the undo layers hold
    CHECK(r.lp.state(1) == TrackState::Playing && r.lp.trackLength(1) == 30 * 48000);
    CHECK(r.lp.undoDepth(0) < depth && r.lp.layerCount(0) == 3);         // given up, still heard
}

int main() {
    trackBufSharesUntilWritten(); audioIgnoresTheBudgetUntilThePoolIsFull();
    onlyRecordedAudioUsesThePool(); aThrownAwayTakeGivesItsBlocksBack(); aFullPoolClosesTheFirstRecording(); aFullPoolClosesARecordingOntoTheLoop();
    loopsWinOverUndo();
    REPORT();
}
