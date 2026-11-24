#include "looper_rig.hpp"
#include "pieces/PieceWorker.hpp"
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>
#include <sys/stat.h>
#include <unistd.h>

// The pieces worker: a save writes the held snapshot to a slot folder while the audio plays on; a load checks the slot,
// asks for the engine and imports the piece. The test plays the audio thread: a block, then poll(), 5 s at most.

using namespace ouro;
using namespace ouro::pieces;
using Result = PieceWorker::Result;
using Clock = std::chrono::steady_clock;

static std::string tmpRoot() {
    char t[] = "build/tests/pwXXXXXX";
    mkdir("build/tests", 0777);
    return mkdtemp(t);
}
static std::string slurp(const std::string& p) {
    std::string s;
    if (FILE* f = fopen(p.c_str(), "rb")) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f))) s.append(b, n); fclose(f); }
    return s;
}
static void put(const std::string& p, const std::string& s) { FILE* f = fopen(p.c_str(), "wb"); fwrite(s.data(), 1, s.size(), f); fclose(f); }
static void putMarker(const std::string& root, int slot) {           // a slot holding "X"
    mkdir(slotDir(root, slot).c_str(), 0777); put(slotDir(root, slot) + "/piece.txt", "X");
}
static bool slotIsMarker(const std::string& root, int slot) {
    return slurp(slotDir(root, slot) + "/piece.txt") == "X" && !exists(slotDir(root, slot) + "/A1.wav") &&
           !exists(slotDir(root, slot) + ".new");
}

// Blocks and poll() until the job's result; the engine lent when asked, and no block while lent. asked: it was.
static Result finish(Rig& r, PieceWorker& w, bool* asked = nullptr) {
    bool lent = false;
    for (const auto end = Clock::now() + std::chrono::seconds(5); Clock::now() < end;) {
        const Result x = w.poll();
        if (x != Result::None) return x;
        if (!lent && w.wantsEngine()) { w.lendEngine(); lent = true; if (asked) *asked = true; }
        if (lent) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        else r.silence(r.block);
    }
    CHECK(!"a result within 5 s");
    return Result::None;
}
// Snapshot, save to slot, the result; the snapshot dropped after it.
static Result save(Rig& r, PieceWorker& w, int slot, PieceMeta* out = nullptr) {
    PieceMeta m;
    CHECK(r.lp.takeSnapshot(m));
    w.startSave(slot, m, r.lp.audioRefusals());
    const Result x = finish(r, w);
    r.lp.dropSnapshot();
    if (out) *out = m;
    return x;
}
// A: A1, A2. B: a copy of A (shared blocks), then B3 at -6 dB. Every track 24000 samples (6 chunks); part B current.
static void recordPiece(Rig& r) {
    recordBase(r, 24000, 0.2f); recordBaseOn(r, 1);
    r.lp.setPart(1); r.silence(24000);
    recordBaseOn(r, 2, 0.1f, 880.0); r.lp.setVolumeDb(2, -6.0f);
    r.silence(1000);
}

static void saveWritesTheSlot() {
    const std::string root = tmpRoot();
    Rig r; recordPiece(r);
    PieceWorker w(r.lp, root);
    PieceMeta m; m.bpm = 120.0f;
    CHECK(r.lp.takeSnapshot(m));
    CHECK(m.track[0][1].len == 24000 && m.track[1][1].len == 24000 && m.track[1][2].len == 24000);
    w.startSave(3, m, r.lp.audioRefusals());
    CHECK(finish(r, w) == Result::Saved);
    const std::string dir = slotDir(root, 3), text = slurp(dir + "/piece.txt");
    PieceMeta f;
    CHECK(parsePiece(text, f) && text == formatPiece(m));
    for (int p = 0; p < kParts; ++p)
        for (int t = 0; t < kTracks; ++t) {
            const uint32_t len = m.track[p][t].len;
            const std::string wav = dir + "/" + trackName(p, t) + ".wav";
            if (!len) { CHECK(!exists(wav)); continue; }
            WavReader rd; std::vector<float> got(len), want(len);
            CHECK(rd.open(wav) && rd.rate() == 48000 && rd.frames() == len && rd.read(got.data(), len));
            r.lp.readSnapshot(p, t, 0, want.data(), len);
            CHECK(std::memcmp(got.data(), want.data(), len * sizeof(float)) == 0);
        }
    CHECK(w.fullSlots() == 1u << 2 && !exists(dir + ".new"));
    r.lp.dropSnapshot();
    removeTree(root);
}

// An overdub on B1 writes blocks the snapshot shares while the worker reads them; watch() sees no refusal.
static void saveWhilePlayingChangesNothingHeard() {
    const std::string root = tmpRoot();
    Rig a, b;
    for (Rig* x : {&a, &b}) recordPiece(*x);
    PieceWorker w(a.lp, root);
    PieceMeta m;
    CHECK(a.lp.takeSnapshot(m));
    w.startSave(1, m, a.lp.audioRefusals());
    for (Rig* x : {&a, &b}) x->lp.pressRec(0);
    const auto in = sine(440, 0.1f, 128);
    size_t blocks = 0;
    Result x = Result::None;
    for (const auto end = Clock::now() + std::chrono::seconds(5); x == Result::None && Clock::now() < end; ++blocks) {
        a.feed(in); w.watch(a.lp.audioRefusals()); x = w.poll();
    }
    for (size_t i = 0; i < blocks; ++i) b.feed(in);
    CHECK(x == Result::Saved && a.out.size() == b.out.size() && maxAbsDiff(a.out, b.out) == 0.0f);
    a.lp.dropSnapshot();
    removeTree(root);
}

static void cancelLeavesTheSlot() {
    const std::string root = tmpRoot();
    putMarker(root, 3);
    Rig r; recordPiece(r);
    PieceWorker w(r.lp, root);
    PieceMeta m;
    CHECK(r.lp.takeSnapshot(m));
    w.startSave(3, m, r.lp.audioRefusals());
    w.cancel();
    CHECK(finish(r, w) == Result::Cancelled);
    CHECK(slotIsMarker(root, 3) && w.fullSlots() == 1u << 2);
    r.lp.dropSnapshot();
    removeTree(root);
}

// The overdub runs into the full pool first, so that the save cannot finish before watch() sees the refusal; the save
// gets the refusal count of its snapshot, as the plugin passes it.
static void refusalCancelsTheSave() {
    const std::string root = tmpRoot();
    putMarker(root, 3);
    Rig r(48000.0, 128, size_t(2) << 20);                               // 128 blocks
    recordBase(r, 100 * 4096, 0.2f); r.silence(1000);                   // A1 in 100 of them
    PieceWorker w(r.lp, root);
    PieceMeta m;
    CHECK(r.lp.takeSnapshot(m));
    const uint32_t atSnapshot = r.lp.audioRefusals();
    r.lp.pressRec(0);                                                   // the overdub copies every block it writes
    const auto in = sine(330, 0.1f, 128);
    for (int i = 0; i < 20000 && r.lp.audioRefusals() == atSnapshot; ++i) r.feed(in);
    CHECK(r.lp.audioRefusals() > atSnapshot);
    w.startSave(3, m, atSnapshot);
    Result x = Result::None;
    for (const auto end = Clock::now() + std::chrono::seconds(5); x == Result::None && Clock::now() < end;) {
        w.watch(r.lp.audioRefusals()); x = w.poll(); r.feed(in);
    }
    CHECK(x == Result::Cancelled && slotIsMarker(root, 3));
    const uint32_t used = r.lp.blocksUsed();
    r.lp.dropSnapshot();
    CHECK(r.lp.blocksUsed() < used);
    removeTree(root);
}

static void saveToAReadOnlyRootFails() {
    const std::string root = tmpRoot();
    putMarker(root, 3);
    chmod(root.c_str(), 0555);
    Rig r; recordPiece(r);
    PieceWorker w(r.lp, root);
    CHECK(save(r, w, 3) == Result::SaveError);
    CHECK(slotIsMarker(root, 3) && w.fullSlots() == 1u << 2);
    chmod(root.c_str(), 0755);
    removeTree(root);
}

static void loadRestoresThePiece() {
    const std::string root = tmpRoot();
    Rig r; recordPiece(r);
    r.lp.stop(StopMode::Instant); r.silence(2000);
    const auto want = r.passFromTheTop();
    PieceWorker w(r.lp, root);
    PieceMeta m;
    CHECK(save(r, w, 4, &m) == Result::Saved);
    r.lp.clearAll(); r.settle();
    CHECK(r.lp.empty());
    w.startLoad(4);
    bool asked = false;
    CHECK(finish(r, w, &asked) == Result::Loaded && asked);
    CHECK(formatPiece(w.loaded()) == formatPiece(m) && w.progress() == 1.0f && w.fullSlots() == 1u << 3);
    CHECK(r.lp.transport() == Transport::Stopped && r.lp.part() == 1 && r.lp.volumeDb(2) == -6.0f);
    const auto got = r.passFromTheTop();
    CHECK(got.size() == want.size() && maxAbsDiff(got, want) < 1e-6f && peak(got) > 0.1f);
    removeTree(root);
}

// Each refused before asking for the engine: an empty slot, another rate, a missing WAV, an L over the track limit.
static void loadChecksBeforeTouching() {
    const std::string root = tmpRoot();
    Rig r; recordPiece(r);
    PieceWorker w(r.lp, root);
    PieceMeta m;
    CHECK(r.lp.takeSnapshot(m));
    for (int s : {6, 7}) { w.startSave(s, m, r.lp.audioRefusals()); CHECK(finish(r, w) == Result::Saved); }
    r.lp.dropSnapshot();
    const std::string t6 = slotDir(root, 6) + "/piece.txt";
    std::string text = slurp(t6);
    CHECK(text.find("rate 48000\n") != std::string::npos);
    text.replace(text.find("rate 48000\n"), 11, "rate 44100\n"); put(t6, text);
    CHECK(unlink((slotDir(root, 7) + "/B2.wav").c_str()) == 0);
    PieceMeta longL; longL.rate = 48000; longL.loop = r.lp.maxTrackLength() + 1;   // no track: nothing else to refuse
    mkdir(slotDir(root, 8).c_str(), 0777); put(slotDir(root, 8) + "/piece.txt", formatPiece(longL));
    const auto image = r.settle();
    const std::pair<int, Result> cases[] = {{1, Result::Empty}, {6, Result::LoadError}, {7, Result::LoadError},
                                            {8, Result::LoadError}};
    for (const auto& c : cases) {
        w.startLoad(c.first);
        bool asked = false;
        CHECK(finish(r, w, &asked) == c.second && !asked);
        CHECK(r.lp.part() == 1 && r.lp.trackLength(2) == 24000 && maxAbsDiff(r.loopImage(), image) < 1e-6f);
    }
    removeTree(root);
}

// B saved as a copy of A shares its blocks; read back, they are apart and need twice the blocks.
static void tooBigLeavesTheEngine() {
    const std::string root = tmpRoot();
    PieceMeta m;
    {
        Rig big; recordBase(big, 40 * 4096, 0.2f);
        big.lp.setPart(1); big.silence(40 * 4096 + 1000);
        CHECK(big.lp.part() == 1);
        PieceWorker w(big.lp, root);
        CHECK(save(big, w, 2, &m) == Result::Saved);
    }
    Rig small(48000.0, 128, size_t(1) << 20);                           // 64 blocks: A fits, A and B apart do not
    CHECK(blocksFor(m) == 80 && blocksFor(m) > small.lp.capacityBlocks());
    recordBase(small, 24000, 0.1f);
    const auto image = small.settle();
    PieceWorker w(small.lp, root);
    w.startLoad(2);
    bool asked = false;
    CHECK(finish(small, w, &asked) == Result::TooBig && !asked);
    CHECK(small.lp.loopLength() == 24000 && maxAbsDiff(small.loopImage(), image) < 1e-6f);
    removeTree(root);
}

// halt() with no audio thread: the save stops and leaves the slot, no result; the worker takes jobs after it.
static void haltStopsASave() {
    const std::string root = tmpRoot();
    putMarker(root, 3);
    Rig r; recordPiece(r);
    PieceWorker w(r.lp, root);
    PieceMeta m;
    CHECK(r.lp.takeSnapshot(m));
    w.startSave(3, m, r.lp.audioRefusals());
    w.halt();
    CHECK(w.idle() && w.poll() == Result::None && slotIsMarker(root, 3));
    w.startSave(3, m, r.lp.audioRefusals());
    CHECK(finish(r, w) == Result::Saved && slurp(slotDir(root, 3) + "/piece.txt") == formatPiece(m));
    r.lp.dropSnapshot();
    removeTree(root);
}

// halt() does not wait for an engine never lent: the load ends untouched, no result.
static void haltEndsALoadWaitingForTheEngine() {
    const std::string root = tmpRoot();
    Rig r; recordPiece(r);
    PieceWorker w(r.lp, root);
    CHECK(save(r, w, 5) == Result::Saved);
    const auto image = r.settle();
    w.startLoad(5);
    for (const auto end = Clock::now() + std::chrono::seconds(5); !w.wantsEngine() && Clock::now() < end;) r.silence(r.block);
    CHECK(w.wantsEngine());
    w.halt();
    CHECK(w.idle() && !w.wantsEngine() && w.poll() == Result::None);
    CHECK(r.lp.part() == 1 && maxAbsDiff(r.loopImage(), image) < 1e-6f);
    w.startLoad(5);
    CHECK(finish(r, w) == Result::Loaded);
    removeTree(root);
}

// No pieces folder (Windows): a save fails, a load finds nothing.
static void noRootDoesNothing() {
    Rig r; recordPiece(r);
    PieceWorker w(r.lp, "");
    CHECK(save(r, w, 1) == Result::SaveError);
    w.startLoad(1);
    CHECK(finish(r, w) == Result::Empty && w.fullSlots() == 0 && r.lp.trackLength(2) == 24000);
}

int main() {
    saveWritesTheSlot(); saveWhilePlayingChangesNothingHeard(); cancelLeavesTheSlot(); refusalCancelsTheSave();
    saveToAReadOnlyRootFails(); loadRestoresThePiece(); loadChecksBeforeTouching(); tooBigLeavesTheEngine();
    haltStopsASave(); haltEndsALoadWaitingForTheEngine(); noRootDoesNothing();
    REPORT();
}
