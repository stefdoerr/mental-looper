#include "looper_rig.hpp"
using ouro::TrackState; using ouro::Transport; using ouro::StopMode;

static const uint32_t L = 24000;
static void toPos(Rig& r, uint32_t p) { r.silence((L + p - r.lp.position()) % L); }   // base phase p
static void silentBase(Rig& r) { r.lp.setTailMs(0); recordBase(r, L, 0.0f); }        // track 0: a silent loop

static void oneLoopTracksReportLengthAndPosition() {
    Rig r; silentBase(r); toPos(r, 6000);
    CHECK(r.lp.trackLength(0) == L && r.lp.trackPosition(0) == r.lp.position() && r.lp.trackLength(1) == 0);
    r.lp.pressRec(1); r.feed(sine(330, 0.1f, L)); r.lp.pressRec(1);   // exactly one loop, from mid-pass
    CHECK(r.lp.trackLength(1) == L);
    for (int i = 0; i < 5; ++i) {
        r.silence(7777);
        CHECK(r.lp.trackPosition(1) == r.lp.position() && r.lp.trackPosition(0) == r.lp.position());
    }
    r.lp.undo(1); r.silence(3000);                                    // the base (a recording onto the loop): kept for redo
    CHECK(r.lp.state(1) == TrackState::Empty && r.lp.trackLength(1) == L && r.lp.trackPosition(1) == r.lp.position());
    r.lp.clearTrack(1); r.silence(1000); CHECK(r.lp.trackLength(1) == 0);
}

int main() {
    oneLoopTracksReportLengthAndPosition();
    REPORT();
}
