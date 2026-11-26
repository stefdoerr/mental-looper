// The face's numbers (hmi/FaceView.hpp): the bar by the display's rule, the loops, the first take's seconds.
#include "looper_rig.hpp"
#include "hmi/FaceView.hpp"
using ouro::faceView;

static const uint32_t L = 24000;
static void toPos(Rig& r, uint32_t p) { r.silence((L + p - r.lp.position()) % L); }   // base phase p

static void emptyTrackFollowsTheLoop() {
    Rig r; recordBase(r, L, 0.1f); r.silence(7000);
    const ouro::FaceView f = faceView(r.lp, 1);
    CHECK(f.progress == float(r.lp.position()) / L && f.loops == 1);
}

static void oneLoopTrack() {
    Rig r; recordBase(r, L, 0.1f); r.silence(7000);
    const ouro::FaceView f = faceView(r.lp, 0);
    CHECK(r.lp.trackLength(0) == L);
    CHECK(f.progress == float(r.lp.trackPosition(0)) / r.lp.trackLength(0) && f.loops == 1);
}

static void fourLoopTrack() {
    Rig r; recordBase(r, L, 0.1f); toPos(r, 0);
    r.lp.pressRec(1); r.feed(sine(220, 0.1f, 4 * L)); r.lp.pressRec(1); r.silence(5000);
    CHECK(r.lp.trackLength(1) == 4 * L);
    const ouro::FaceView f = faceView(r.lp, 1);
    CHECK(f.loops == 4 && f.progress == float(r.lp.trackPosition(1)) / (4 * L));
}

static void firstTakeShowsSeconds() {
    Rig r; r.lp.pressRec(0); r.feed(sine(220, 0.1f, 72000));       // 1.5 s
    ouro::FaceView f = faceView(r.lp, 0);
    CHECK(f.looper == ouro::kFaceFirstTake && f.takeSecs == 1 && f.progress == 0.0f && f.loops == 1);
    r.lp.pressRec(0); r.silence(128);
    f = faceView(r.lp, 0);
    CHECK(f.takeSecs == 0 && f.looper != ouro::kFaceFirstTake);
}

static void faceWaitsWhileANewPartWaits() {
    Rig r; recordBase(r, L, 0.1f); toPos(r, 100); r.lp.newPart(1);
    CHECK(faceView(r.lp, 0).looper == ouro::kFaceWaiting);
    r.silence(L); r.silence(256);   // the switch
    CHECK(faceView(r.lp, 0).looper == ouro::kFaceWriting);
}

int main() {
    emptyTrackFollowsTheLoop();
    oneLoopTrack();
    fourLoopTrack();
    firstTakeShowsSeconds();
    faceWaitsWhileANewPartWaits();
    REPORT();
}
