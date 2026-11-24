#include "looper_rig.hpp"
using ouro::TrackState;

// Saved pieces, the engine side: a snapshot a save reads while the audio plays on, an import a load writes, and the
// change count that tells whether a load would lose work.

static std::vector<float> readTrack(Rig& r, int p, int t, uint32_t len) {
    std::vector<float> s(len);
    r.lp.readSnapshot(p, t, 0, s.data(), len);
    return s;
}
// Stopped: one pass of track t alone from its beginning (the second after the restart: the first has the fade-in).
static std::vector<float> passAlone(Rig& r, int t) {
    for (int u = 0; u < 4; ++u) r.lp.setOn(u, u == t);
    r.lp.restart();
    const uint32_t len = r.lp.trackLength(t);
    r.silence(len);
    auto o = r.capture(len);
    r.lp.stop(ouro::StopMode::Instant); r.silence(2000);
    return o;
}
// Snapshot in a, import into b in 4096-sample chunks, as a load would.
static ouro::PieceMeta importInto(Rig& a, Rig& b) {
    ouro::PieceMeta m;
    CHECK(a.lp.takeSnapshot(m));
    b.lp.importBegin();
    std::vector<float> buf(4096);
    for (int p = 0; p < ouro::kParts; ++p)
        for (int t = 0; t < ouro::kTracks; ++t)
            for (uint32_t i = 0, len = m.track[p][t].len; i < len; i += 4096) {
                const uint32_t n = std::min(4096u, len - i);
                a.lp.readSnapshot(p, t, i, buf.data(), n);
                CHECK(b.lp.importTrack(p, t, i, buf.data(), n));
            }
    b.lp.importEnd(m);
    a.lp.dropSnapshot();
    return m;
}

static void snapshotReadsFromTheTrackBeginning() {
    Rig r; recordBase(r, 24000, 0.2f);
    r.silence(9000);                                                     // mid-loop: track 1's phase is not 0
    r.lp.pressRec(1); r.feed(sine(330, 0.1f, 2 * 24000 + 1000)); r.lp.pressRec(1);   // 2 L, rounded down: a fold op
    r.silence(1000);                                                     // the punch-out fade ends, the fold waits
    CHECK(r.lp.trackLength(1) == 48000 && r.lp.pendingOps(1) == 1);
    ouro::PieceMeta m; CHECK(r.lp.takeSnapshot(m));
    const auto s0 = readTrack(r, 0, 0, 24000), s1 = readTrack(r, 0, 1, 48000);
    r.lp.stop(ouro::StopMode::Instant); r.settle();
    CHECK(maxAbsDiff(passAlone(r, 0), s0) < 1e-6f && maxAbsDiff(passAlone(r, 1), s1) < 1e-6f);
    CHECK(peak(s0) > 0.1f && peak(s1) > 0.05f);
    std::vector<float> mid(100); r.lp.readSnapshot(0, 1, 47950, mid.data(), 100);   // from a sample on, wrapping
    CHECK(maxAbsDiff(mid, slice(s1, 47950, 48000)) == 0.0f);
    CHECK(maxAbsDiff(slice(mid, 50, 100), slice(s1, 0, 50)) == 0.0f);
}

static void snapshotResolvesPendingOps() {
    Rig r; recordBase(r, 24000, 0.2f);
    r.lp.pressRec(0); r.feed(sine(330, 0.1f, 12000)); r.lp.pressRec(0); r.silence(1000);   // a layer, its fade done
    const auto dubbed = r.loopImage();
    r.lp.undo(0);
    CHECK(r.lp.pendingOps(0) == 1);
    ouro::PieceMeta m; CHECK(r.lp.takeSnapshot(m));
    const auto s = readTrack(r, 0, 0, 24000);
    CHECK(maxAbsDiff(s, r.settle()) < 1e-6f && maxAbsDiff(s, dubbed) > 0.05f);
}

// The piece at the snapshot, whatever the engine does after; at the drop the engine holds what the same session without
// a snapshot holds. A new recording reuses every block given back: one the snapshot still read would change.
static void snapshotIgnoresLaterChanges() {
    Rig r, twin;
    for (Rig* x : {&r, &twin}) {
        recordBase(*x, 24000, 0.2f); recordBaseOn(*x, 1);
        x->lp.pressRec(1); x->feed(sine(330, 0.1f, 12000)); x->lp.pressRec(1); x->silence(1000);
        x->lp.undo(1);                                                   // an op pending on track 1 reads the layer
    }
    ouro::PieceMeta m; CHECK(r.lp.takeSnapshot(m));
    const auto s0 = readTrack(r, 0, 0, 24000), s1 = readTrack(r, 0, 1, 24000);
    for (Rig* x : {&r, &twin}) {
        x->lp.pressRec(0); x->feed(sine(440, 0.1f, 12000)); x->lp.pressRec(0);   // writes track 0's shared blocks
        x->silence(1000); x->lp.undo(1);
        x->lp.clearAll(); x->settle();
        CHECK(x->lp.loopLength() == 0);                                  // every block of the live tracks went back
        recordBase(*x, 40 * 4096, 0.3f);
    }
    CHECK(maxAbsDiff(readTrack(r, 0, 0, 24000), s0) == 0.0f && maxAbsDiff(readTrack(r, 0, 1, 24000), s1) == 0.0f);
    CHECK(r.lp.blocksUsed() > twin.lp.blocksUsed());
    r.lp.dropSnapshot();
    CHECK(!r.lp.snapshotHeld() && r.lp.blocksUsed() == twin.lp.blocksUsed());
}

static void takeSnapshotWaitsForASettledEngine() {
    Rig r; ouro::PieceMeta m;
    r.lp.pressRec(0); r.feed(sine(220, 0.2f, 24000));
    CHECK(!r.lp.settled() && !r.lp.takeSnapshot(m));                   // the first recording
    r.lp.pressRec(0); r.silence(1000);
    CHECK(!r.lp.takeSnapshot(m));                                        // its tail (50 ms)
    r.silence(2000);
    CHECK(r.lp.takeSnapshot(m) && r.lp.snapshotHeld());
    CHECK(!r.lp.takeSnapshot(m));                                        // one is held
    r.lp.dropSnapshot();
    r.lp.pressRec(1); r.silence(1000); CHECK(!r.lp.takeSnapshot(m));    // a recording onto the loop
    r.silence(23000); r.lp.pressRec(1); r.silence(1000);
    r.lp.pressRec(0); r.feed(sine(330, 0.1f, 1000)); CHECK(!r.lp.takeSnapshot(m));   // an overdub (not silent)
    r.lp.pressRec(0); r.silence(100); CHECK(!r.lp.takeSnapshot(m));     // its punch-out fade (5 ms)
    r.silence(200);
    r.lp.setPart(1); r.silence(24000 - r.lp.position());                // the switch, on the part end
    CHECK(r.lp.part() == 1 && !r.lp.takeSnapshot(m));                   // part A still ramps out
    r.silence(200);
    CHECK(r.lp.settled() && r.lp.takeSnapshot(m));
    r.lp.dropSnapshot();
}

static void snapshotMetaHoldsTheMix() {
    Rig r; recordBase(r, 24000, 0.2f);
    r.lp.setPart(1); r.silence(24000);                                   // B: a copy of A
    recordBaseOn(r, 2); r.silence(1000);
    r.lp.setVolumeDb(2, -6.0f); r.lp.setOn(3, false);
    r.lp.setStep(0, 1, 2); r.lp.setSong(true);
    ouro::PieceMeta m; m.bpm = 120.0f;
    CHECK(r.lp.takeSnapshot(m));
    CHECK(m.rate == 48000 && m.loop == r.lp.loopLength() && m.bars == r.lp.bars() && m.bpm == 120.0f);
    CHECK(m.part == 1 && m.song && m.stepPart[0] == 1 && m.stepReps[0] == 2);
    CHECK(m.stepPart[1] == ouro::kStepLoop && m.stepReps[1] == 1);
    for (int t = 0; t < 4; ++t) {
        CHECK(m.track[1][t].len == r.lp.trackLength(t));
        CHECK(m.track[1][t].volDb == (t == 2 ? -6.0f : 0.0f) && m.track[1][t].on == (t != 3));
        CHECK(m.track[0][t].len == (t == 0 ? 24000u : 0u) && m.track[0][t].on && m.track[0][t].volDb == 0.0f);
        CHECK(m.track[2][t].len == 0 && m.track[3][t].len == 0);
    }
    CHECK(m.track[1][0].len == 24000 && m.track[1][2].len == 24000);
    r.lp.dropSnapshot();
}

static void importPlaysBackExactly() {
    Rig a, b;
    recordBase(a, 24000, 0.2f);
    a.lp.pressRec(0); a.feed(sine(330, 0.05f, 12000)); a.lp.pressRec(0);   // a layer on A1
    a.silence((33000 - a.lp.position()) % 24000);                     // to position 9000
    a.lp.pressRec(1); a.feed(sine(550, 0.1f, 48000)); a.lp.pressRec(1);    // A2: 2 L from mid-loop
    a.lp.setVolumeDb(1, -6.0f);
    a.lp.setPart(1); a.silence(48000);                                     // B: a copy of A
    recordBaseOn(a, 2, 0.1f, 880.0); a.lp.setOn(3, false);
    a.lp.pressRec(1); a.feed(sine(660, 0.05f, 6000)); a.lp.pressRec(1); a.silence(1000);
    a.lp.undo(1);                                                          // pending at the snapshot
    a.lp.setStep(0, 1, 2); a.lp.setSong(true);
    CHECK(a.lp.part() == 1 && a.lp.pendingOps(1) == 1);
    const ouro::PieceMeta m = importInto(a, b);
    CHECK(b.lp.transport() == ouro::Transport::Stopped && b.lp.part() == 1 && b.lp.songOn());
    CHECK(b.lp.loopLength() == 24000 && b.lp.bars() == a.lp.bars() && b.lp.stepRepeats(0) == 2);
    for (int p = 1; p >= 0; --p) {                                         // B, then A
        if (p == 0) { a.lp.setPart(0); b.lp.setPart(0); }
        for (int t = 0; t < 4; ++t) {
            CHECK(b.lp.trackLength(t) == a.lp.trackLength(t) && b.lp.trackLength(t) == m.track[p][t].len);
            CHECK(b.lp.undoDepth(t) == 0 && b.lp.isOn(t) == a.lp.isOn(t) && b.lp.volumeDb(t) == a.lp.volumeDb(t));
        }
        if (p == 1) { a.lp.stop(ouro::StopMode::Instant); a.silence(2000); }
        const auto pa = a.passFromTheTop(), pb = b.passFromTheTop();
        CHECK(pa.size() == 48000 && maxAbsDiff(pa, pb) < 1e-6f && peak(pb) > 0.1f);
    }
    b.lp.importBegin();                                                    // a failed load ends here: every part empty
    CHECK(b.lp.transport() == ouro::Transport::Running && b.lp.empty() && b.lp.loopLength() == 0);
    recordBase(b, 24000, 0.1f); CHECK(b.lp.loopLength() == 24000);       // the first press records
}

static void importedPieceFitsCapacity() {
    Rig r; recordBase(r, 24000, 0.2f); recordBaseOn(r, 1); r.silence(1000);
    ouro::PieceMeta m; CHECK(r.lp.takeSnapshot(m));
    CHECK(ouro::blocksFor(m) == 12 && ouro::blocksFor(m) <= r.lp.capacityBlocks());
    CHECK(r.lp.maxTrackLength() == 300u * 48000u);
    r.lp.dropSnapshot();
    Rig small(48000.0, 128, size_t(1) << 20);
    CHECK(small.lp.capacityBlocks() == 64 && small.lp.audioRefusals() == 0);
    small.lp.importBegin();
    std::vector<float> chunk(4096, 0.1f);
    for (uint32_t k = 0; k < 64; ++k) CHECK(small.lp.importTrack(0, 0, k * 4096, chunk.data(), 4096));
    CHECK(!small.lp.importTrack(0, 0, 64 * 4096, chunk.data(), 4096) && small.lp.audioRefusals() == 1);
}

static void changesCountsWhatASaveWrites() {
    Rig r;
    uint32_t c = r.lp.changes();
    auto rose = [&] { const bool up = r.lp.changes() != c; c = r.lp.changes(); return up; };
    recordBase(r, 24000, 0.2f); CHECK(rose());                          // a close
    r.lp.pressRec(0); r.feed(sine(330, 0.1f, 6000)); r.lp.pressRec(0); CHECK(rose());
    r.silence(1000);
    r.lp.undo(0); CHECK(rose());
    r.lp.setVolumeDb(0, -3.0f); CHECK(rose());
    r.lp.setVolumeDb(0, -3.0f); CHECK(!rose());
    r.lp.setStep(1, 1, 2); CHECK(rose());
    r.lp.setStep(1, 1, 2); CHECK(!rose());
    r.lp.setPart(1); r.silence(24000); CHECK(r.lp.part() == 1 && rose());   // a copy on entry
    r.lp.setPart(0); r.silence(24000); CHECK(r.lp.part() == 0 && !rose());  // B is not empty: no copy
    r.silence(10 * 24000); CHECK(!rose());
    CHECK(!r.lp.empty());
    r.lp.clearAll(); CHECK(rose()); r.settle(); CHECK(r.lp.empty());
}

int main() {
    snapshotReadsFromTheTrackBeginning(); snapshotResolvesPendingOps(); snapshotIgnoresLaterChanges();
    takeSnapshotWaitsForASettledEngine(); snapshotMetaHoldsTheMix(); importPlaysBackExactly();
    importedPieceFitsCapacity(); changesCountsWhatASaveWrites();
    REPORT();
}
