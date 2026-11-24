#include "test_util.hpp"
#include "signals.hpp"
#include "dsp/Piece.hpp"
#include "pieces/PieceFile.hpp"
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

using namespace ouro;
using namespace ouro::pieces;

static std::string tmpRoot() {
    char t[] = "build/tests/pfXXXXXX";
    mkdir("build/tests", 0777);
    return mkdtemp(t);
}
static bool isDir(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
static void put(const std::string& p, const std::string& s) { FILE* f = fopen(p.c_str(), "wb"); fwrite(s.data(), 1, s.size(), f); fclose(f); }
static void le32(std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s += char(v >> (8 * i)); }
static void le16(std::string& s, uint16_t v) { s += char(v); s += char(v >> 8); }
static std::string wavBytes(uint16_t fmt, uint16_t ch, uint16_t bits, uint32_t dataBytes, uint32_t have, bool list) {
    std::string s = "RIFF"; le32(s, 36 + dataBytes); s += "WAVEfmt "; le32(s, 16);
    le16(s, fmt); le16(s, ch); le32(s, 48000); le32(s, 48000 * ch * bits / 8); le16(s, ch * bits / 8); le16(s, bits);
    if (list) { s += "LIST"; le32(s, 5); s += "abcde"; s += '\0'; }   // odd size: padded
    s += "data"; le32(s, dataBytes); s += std::string(have, '\0');
    return s;
}

static void wavRoundTripIsExact() {
    std::string root = tmpRoot(), p = root + "/a.wav";
    std::vector<float> x = sine(220, 0.3f, 10000); x.push_back(-0.0f); x.push_back(1e-30f);
    WavWriter w; CHECK(w.open(p, 48000));
    CHECK(w.write(x.data(), 5000) == Err::None && w.write(x.data() + 5000, 5002) == Err::None && w.close() == Err::None);
    WavReader r; CHECK(r.open(p) && r.rate() == 48000 && r.frames() == 10002);
    std::vector<float> y(10002); CHECK(r.read(y.data(), 10002));
    CHECK(std::memcmp(x.data(), y.data(), 10002 * sizeof(float)) == 0);
    removeTree(root);
}
static void wavReaderRejectsOtherFormats() {
    std::string root = tmpRoot(), p = root + "/b.wav"; WavReader r;
    put(p, wavBytes(1, 1, 16, 8, 8, false)); CHECK(!r.open(p));
    put(p, wavBytes(3, 2, 32, 16, 16, false)); CHECK(!r.open(p));
    put(p, wavBytes(3, 1, 32, 16, 8, false)); CHECK(!r.open(p));
    put(p, wavBytes(3, 1, 32, 8, 8, false)); CHECK(r.open(p) && r.frames() == 2);
    removeTree(root);
}
static void wavReaderSkipsUnknownChunks() {
    std::string root = tmpRoot(), p = root + "/c.wav"; WavReader r;
    std::string s = wavBytes(3, 1, 32, 8, 0, true); float v[2] = {0.5f, -0.25f};
    s.append((const char*)v, 8); put(p, s);
    float o[2]; CHECK(r.open(p) && r.frames() == 2 && r.read(o, 2) && o[0] == 0.5f && o[1] == -0.25f);
    removeTree(root);
}

static PieceMeta sample() {
    PieceMeta m; m.rate = 48000; m.loop = 96000; m.bars = 4; m.bpm = 120; m.part = 1; m.song = true;
    m.stepPart[0] = 0; m.stepReps[0] = 2; m.stepPart[1] = 1; m.stepReps[1] = 1; m.stepPart[2] = 0; m.stepReps[2] = 2;
    m.track[0][0].len = 192000; m.track[0][0].volDb = -3.5f; m.track[0][0].on = true;
    m.track[1][2].len = 96000; m.track[1][2].on = false;
    return m;
}
static void pieceTextRoundTrip() {
    PieceMeta m = sample(), n;
    std::string t = formatPiece(m); CHECK(parsePiece(t, n));
    CHECK(n.rate == m.rate && n.loop == m.loop && n.bars == m.bars && n.bpm == m.bpm && n.part == m.part && n.song == m.song);
    for (int i = 0; i < kSteps; ++i) CHECK(n.stepPart[i] == m.stepPart[i] && n.stepReps[i] == m.stepReps[i]);
    for (int p = 0; p < kParts; ++p) for (int k = 0; k < kTracks; ++k)
        CHECK(n.track[p][k].len == m.track[p][k].len && n.track[p][k].volDb == m.track[p][k].volDb && n.track[p][k].on == m.track[p][k].on);
    std::string want = "ouroboros-piece 1\nrate 48000\nloop 96000\nbars 4\nbpm 120.000\npart 1\nsong 1\n"
        "steps 0:2 1:1 0:2 4:1 4:1 4:1 4:1 4:1 4:1 4:1 4:1 4:1 4:1 4:1 4:1 4:1\ntrack A1 192000 -3.50 1\n";
    CHECK(t.compare(0, want.size(), want) == 0);
}
static std::string replaceOnce(std::string s, const std::string& a, const std::string& b) {
    size_t i = s.find(a); if (i != std::string::npos) s.replace(i, a.size(), b); return s;
}
static void parsePieceRejects() {
    std::string t = formatPiece(sample()); PieceMeta n;
    CHECK(parsePiece(t, n));
    CHECK(!parsePiece(replaceOnce(t, "piece 1", "piece 2"), n));
    CHECK(!parsePiece(replaceOnce(t, "bars 4\n", ""), n));
    CHECK(!parsePiece(replaceOnce(t, "track A1 192000", "track A1 100000"), n));
    CHECK(!parsePiece(replaceOnce(t, "part 1", "part 4"), n));
    CHECK(!parsePiece(replaceOnce(t, "steps 0:2 1:1", "steps 0:2 6:1"), n));
    CHECK(!parsePiece(replaceOnce(t, "steps 0:2 1:1", "steps 0:2 1:0"), n));
    CHECK(!parsePiece(replaceOnce(t, "loop 96000", "loop 0"), n));
}
static void recoverSlotsAfterACrash() {
    std::string root = tmpRoot();
    mkdir((slotDir(root, 3) + ".old").c_str(), 0777); put(slotDir(root, 3) + ".old/piece.txt", "x");   // (a)
    mkdir(slotDir(root, 4).c_str(), 0777); mkdir((slotDir(root, 4) + ".old").c_str(), 0777);          // (b)
    mkdir((slotDir(root, 5) + ".new").c_str(), 0777);                                                 // (c)
    recoverSlots(root);
    CHECK(isDir(slotDir(root, 3)) && !isDir(slotDir(root, 3) + ".old"));
    CHECK(isDir(slotDir(root, 4)) && !isDir(slotDir(root, 4) + ".old") && !isDir(slotDir(root, 5) + ".new"));
    CHECK(scanSlots(root) == 4u);   // bit 2: slot 3 only
    removeTree(root);
}
static void commitReplacesAtomically() {
    std::string root = tmpRoot();
    mkdir(slotDir(root, 2).c_str(), 0777); put(slotDir(root, 2) + "/piece.txt", "X");
    std::string st = stagingDir(root, 2); CHECK(isDir(st)); put(st + "/piece.txt", "Y");
    CHECK(commitSlot(root, 2) == Err::None);
    FILE* f = fopen((slotDir(root, 2) + "/piece.txt").c_str(), "rb"); char c = 0;
    CHECK(f && fread(&c, 1, 1, f) == 1 && c == 'Y'); if (f) fclose(f);
    CHECK(!isDir(slotDir(root, 2) + ".old") && !isDir(slotDir(root, 2) + ".new") && scanSlots(root) == 2u);
    removeTree(root);
}
static void blocksForCountsWholeBlocks() {
    PieceMeta m; m.track[0][0].len = 4096; m.track[0][1].len = 4097; m.track[2][3].len = 0;
    CHECK(blocksFor(m) == 3);
}

int main() {
    wavRoundTripIsExact(); wavReaderRejectsOtherFormats(); wavReaderSkipsUnknownChunks();
    pieceTextRoundTrip(); parsePieceRejects(); recoverSlotsAfterACrash(); commitReplacesAtomically();
    blocksForCountsWholeBlocks();
    REPORT();
}
