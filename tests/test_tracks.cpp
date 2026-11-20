#include "looper_rig.hpp"

// Track t alone (the others off), settled.
static std::vector<float> soloImage(Rig& r, int t) {
    for (int u = 0; u < ouro::kTracks; ++u) r.lp.setOn(u, u == t);
    return r.settle();
}

static void secondTrackLockedToLoop() {
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.0f); r.silence(5000);
    r.lp.pressRec(1); CHECK(r.lp.state(1) == ouro::TrackState::Recording);
    r.feed(sine(1000, 0.3f, 480)); r.silence(16000); r.lp.pressRec(1);
    CHECK(r.lp.state(1) == ouro::TrackState::Playing && r.lp.loopLength() == 24064);
    auto a = r.loopImage(); r.silence(10 * 24064); CHECK(maxAbsDiff(a, r.loopImage()) < 1e-6f);
}
static void handoverClosesAndStarts() {
    Rig r; r.lp.pressRec(0); r.feed(sine(220, 0.1f, 24064)); r.lp.pressRec(1);
    CHECK(r.lp.state(0) == ouro::TrackState::Playing && r.lp.state(1) == ouro::TrackState::Recording);
    CHECK(r.lp.loopLength() == 24064);
}
static void handoverSplitKeepsUnity() {
    Rig r; auto in = sine(220, 0.1f, 48000);                        // tail 50 ms = 2400
    r.lp.pressRec(0); r.feed(slice(in, 0, 24064)); r.lp.pressRec(1);
    r.feed(slice(in, 24064, 24064 + 4864)); r.lp.pressRec(1);
    auto img = r.settle();
    for (uint32_t i = 0; i < 2400; ++i)
        CHECK(std::fabs(img[i] - (in[i] * fadeInGain(i) + in[24064 + i])) < 1e-5f);
}
static void handoverTailZeroKeepsBaseSmooth() {                    // tail 0: a 3 ms split, t0 alone has no step
    Rig r; r.lp.setTailMs(0); auto in = sine(220, 0.1f, 48000);
    r.lp.pressRec(0); r.feed(slice(in, 0, 24100)); r.lp.pressRec(1);   // closed mid-wave
    r.feed(slice(in, 24100, 24100 + 4800)); r.lp.pressRec(1);
    auto img = r.settle(); float err = 0.0f;
    for (uint32_t i = 0; i < 4800; ++i) err = std::max(err, std::fabs(img[i] - (in[i] * fadeInGain(i) + in[24100 + i])));
    CHECK(err < 1e-5f);
    CHECK(maxStep(wrap(soloImage(r, 0))) < 2.5f * sineStep(220, 0.1f));
}
static void handoverAtPunchOutKeepsUnity() {                        // t0 overdubbing -> t1, inside t0's 5 ms fade
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.0f); recordBaseOn(r, 1, 0.0f); r.settle();   // silent loops
    const uint32_t L = 24064, p0 = r.lp.position(); auto in = sine(330, 0.1f, 9600);
    r.lp.pressRec(0); r.feed(slice(in, 0, 4800)); r.lp.pressRec(1); r.feed(slice(in, 4800, 9600)); r.lp.pressRec(1);
    auto img = r.settle(); float err = 0.0f;
    for (uint32_t i = 144; i < 9600; ++i) err = std::max(err, std::fabs(img[(p0 + i) % L] - in[i]));   // past t0's fade-in
    CHECK(err < 1e-6f);                                             // g to t0 + (1-g) to t1 = unity
    CHECK(maxStep(wrap(soloImage(r, 0))) < 2.5f * sineStep(330, 0.1f));
    CHECK(maxStep(wrap(soloImage(r, 1))) < 2.5f * sineStep(330, 0.1f));
}
static void onOffRampsAndResumesInPlace() {
    Rig r; r.lp.setTailMs(0); auto base = recordBase(r, 24064, 0.1f);
    r.lp.setOn(0, false); auto o = r.capture(2400);
    CHECK(maxStep(o) < 2.5f * sineStep(220, 0.1f) && peak(slice(o, 1920, 2400)) < 1e-6f);
    r.silence(8000); r.lp.setOn(0, true); r.silence(960);
    uint32_t p = r.lp.position(); auto o2 = r.capture(1000);
    for (uint32_t i = 0; i < 1000; ++i) CHECK(std::fabs(o2[i] - base[(p + i) % 24064]) < 1e-6f);
}
static void volumeIsPlaybackOnly() {
    Rig r; r.lp.setTailMs(0); auto base = recordBase(r, 24064, 0.1f);
    r.lp.setVolumeDb(0, -6.0f); auto img = r.settle();
    for (uint32_t i = 0; i < 24064; i += 97) CHECK(std::fabs(img[i] - base[i] * 0.50119f) < 1e-5f);
    r.lp.pressRec(0); r.feed(sine(330, 0.1f, 4800)); r.lp.pressRec(0); r.lp.undo(0);
    CHECK(maxAbsDiff(r.settle(), img) < 1e-5f);
}
static void softClipOnlyAboveKnee() {
    {   // two tracks of amplitude 0.3 (sum 0.6 < knee): exact sum
        Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.3f); recordBaseOn(r, 1, 0.3f);
        auto a = soloImage(r, 0), b = soloImage(r, 1);
        r.lp.setOn(0, true); auto ab = r.settle();
        float err = 0.0f;
        for (uint32_t i = 0; i < 24064; ++i) err = std::max(err, std::fabs(ab[i] - (a[i] + b[i])));
        CHECK(err < 1e-6f && peak(ab) > 0.5f);
    }
    {   // four tracks of amplitude 0.4 at +6 dB: |out| <= 1 everywhere, and samples whose raw sum < knee are unchanged
        Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.4f);
        for (int t = 1; t < 4; ++t) recordBaseOn(r, t, 0.4f, 220.0 + 110.0 * t);
        const float g = std::pow(10.0f, 6.0f / 20.0f);
        std::vector<float> raw(24064, 0.0f);
        for (int t = 0; t < 4; ++t) {                                // each alone at 0 dB is below the knee: exact
            auto s = soloImage(r, t);
            for (uint32_t i = 0; i < 24064; ++i) raw[i] += g * s[i];
        }
        for (int t = 0; t < 4; ++t) { r.lp.setOn(t, true); r.lp.setVolumeDb(t, 6.0f); }
        auto out = r.settle();
        uint32_t below = 0, above = 0; float err = 0.0f;
        for (uint32_t i = 0; i < 24064; ++i) {
            if (std::fabs(raw[i]) < ouro::kSoftClipKnee) { ++below; err = std::max(err, std::fabs(out[i] - raw[i])); }
            else ++above;
        }
        CHECK(peak(out) <= 1.0f && err < 1e-5f && below > 1000 && above > 1000);
    }
}
static void dryThruAndBypass() {
    Rig r; auto s = sine(220, 0.1f, 4800);
    r.feed(s); CHECK(peak(slice(r.out, 0, 4800)) == 0.0f);
    r.lp.setDryThru(true); r.lp.snapGains(); r.feed(s);
    CHECK(maxAbsDiff(slice(r.out, 4800, 9600), s) < 1e-7f);
    // bypass with a loop playing: loops ramp out, dry stays; un-bypass resumes at position() (as in onOff test)
    r.lp.setTailMs(0); auto base = recordBase(r, 24064, 0.1f);
    r.lp.setBypass(true); auto o = r.capture(2400);
    CHECK(maxStep(o) < 2.5f * sineStep(220, 0.1f) && peak(slice(o, 1920, 2400)) < 1e-6f);
    auto d = sine(330, 0.1f, 4800); const size_t from = r.out.size(); r.feed(d);
    CHECK(maxAbsDiff(slice(r.out, from, from + 4800), d) < 1e-7f);
    r.lp.setBypass(false); r.silence(960);
    uint32_t p = r.lp.position(); auto o2 = r.capture(1000);
    for (uint32_t i = 0; i < 1000; ++i) CHECK(std::fabs(o2[i] - base[(p + i) % 24064]) < 1e-6f);
}
static void clearAllResetsAndOverwrites() {
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.5f); r.lp.clearAll();
    CHECK(maxStep(r.capture(2400)) < 2.5f * sineStep(220, 0.5f));
    CHECK(r.lp.loopLength() == 0 && r.lp.state(0) == ouro::TrackState::Empty);
    auto q = sine(220, 0.05f, 16000); r.lp.pressRec(0); r.feed(q); r.lp.pressRec(0);
    auto img = r.loopImage();
    for (uint32_t i = 0; i < 16000; ++i) CHECK(std::fabs(img[i] - q[i] * fadeInGain(i)) < 1e-6f);
}
static void recordOnEmptyTrackZeroesReusedBuffer() {
    // record t1 loud (amp 0.5), undo t1's base, record t1 again with a 480-sample burst only, mute t0:
    // the image is silent outside [burst start, burst end + 240]
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.1f); recordBaseOn(r, 1, 0.5f); r.settle();
    r.lp.undo(1); r.lp.setOn(0, false); r.silence(3000);
    r.lp.setTriggerLatencyMs(2.0f);                                 // the 96 back-dated positions are zeroed at the press
    const uint32_t L = 24064, p0 = r.lp.position();
    r.lp.pressRec(1); r.feed(sine(1000, 0.3f, 480)); r.lp.pressRec(1);
    CHECK(!r.lp.canRedo(1));                                        // the old base is gone for good
    auto img = r.settle(); float outside = 0.0f;
    for (uint32_t p = 0; p < L; ++p)
        if ((p + L - p0) % L >= 480 + 240) outside = std::max(outside, std::fabs(img[p]));
    CHECK(outside == 0.0f && peak(img) > 0.25f);
    r.lp.redo(1); CHECK(maxAbsDiff(r.settle(), img) == 0.0f);
}
static void baseUndoRedoAndLengthReset() {
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.1f); recordBaseOn(r, 1); auto both = r.settle();
    r.lp.undo(1); CHECK(r.lp.state(1) == ouro::TrackState::Empty && r.lp.canRedo(1));
    r.lp.redo(1); CHECK(maxAbsDiff(r.settle(), both) < 1e-5f);
    r.lp.undo(1); r.lp.undo(0); r.settle();
    CHECK(r.lp.loopLength() == 0 && !r.lp.canRedo(0) && !r.lp.canRedo(1));
}
static void offTrackStillRecords() {
    Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.0f); r.lp.setOn(1, false);
    r.lp.pressRec(1); r.feed(sine(1000, 0.3f, 4800)); r.lp.pressRec(1);
    CHECK(peak(r.settle()) == 0.0f); r.lp.setOn(1, true); CHECK(peak(r.settle()) > 0.25f);
}
static void disabledEngineOutputsDryOnly() {                        // init failed: the dry path only, actions ignored
    ouro::Looper lp; CHECK(!lp.init(1.0e12, 128));
    float in[128], out[128]; std::fill(in, in + 128, 0.3f);
    lp.process(in, out, 128); CHECK(peak(std::vector<float>(out, out + 128)) == 0.0f);
    lp.setDryThru(true); lp.snapGains(); lp.pressRec(0); lp.process(in, out, 128);
    CHECK(maxAbsDiff(std::vector<float>(out, out + 128), std::vector<float>(in, in + 128)) == 0.0f);
    CHECK(lp.state(0) == ouro::TrackState::Empty);
}
static void stressRandomPresses(float trigMs, bool recToDub) {     // Review Focus 3
    // fixed-seed LCG, 2000 blocks of sine input; each block one random action on a random track among
    // pressRec, undo, redo, toggleOn, clearTrack, clearAll, or nothing (mostly nothing, so loops can form).
    // After every block (R5): output finite and |y| <= 1; pendingOps(t) <= kMaxPendingOps;
    // any track Playing or Overdubbing => loopLength() > 0. At the end every block is back in the pool.
    Rig r; r.lp.setBudgetMb(0.0625f);                               // 4 blocks: eviction under the presses too
    r.lp.setTriggerLatencyMs(trigMs); r.lp.setRecToDub(recToDub);
    const uint32_t B = 128, N = 2000;
    auto in = sine(220, 0.5f, B * N);
    uint32_t seed = 1;
    auto rnd = [&](uint32_t n) { seed = seed * 1664525u + 1013904223u; return (seed >> 8) % n; };
    bool bounded = true, opsCapped = true, lengthSet = true;
    uint32_t looping = 0, dubbing = 0, recOnLoop = 0;
    for (uint32_t b = 0; b < N; ++b) {
        const int t = int(rnd(4));
        const uint32_t a = rnd(256);
        if (a < 8) r.lp.pressRec(t);
        else if (a < 12) r.lp.undo(t);
        else if (a < 14) r.lp.redo(t);
        else if (a < 16) r.lp.toggleOn(t);
        else if (a < 18) r.lp.clearTrack(t);
        else if (a < 19) r.lp.clearAll();
        const size_t from = r.out.size();
        r.feed(slice(in, b * B, (b + 1) * B));
        for (size_t i = from; i < r.out.size(); ++i) bounded &= std::isfinite(r.out[i]) && std::fabs(r.out[i]) <= 1.0f;
        bool live = false;
        for (int u = 0; u < ouro::kTracks; ++u) {
            const ouro::TrackState s = r.lp.state(u);
            opsCapped &= r.lp.pendingOps(u) <= ouro::kMaxPendingOps;
            live |= s == ouro::TrackState::Playing || s == ouro::TrackState::Overdubbing;
            dubbing += s == ouro::TrackState::Overdubbing;
            recOnLoop += s == ouro::TrackState::Recording && r.lp.loopLength() > 0;
        }
        lengthSet &= !live || r.lp.loopLength() > 0;
        looping += r.lp.loopLength() > 0;
    }
    CHECK(bounded && opsCapped && lengthSet);
    CHECK(looping > 500 && dubbing > 50 && recOnLoop > 50);         // the presses did reach loops, dubs, recordings on L
    r.lp.clearAll(); r.silence(2000);
    CHECK(r.lp.loopLength() == 0 && r.lp.undoBytes() == 0);
    for (int u = 0; u < ouro::kTracks; ++u) CHECK(r.lp.state(u) == ouro::TrackState::Empty);
}

int main() {
    secondTrackLockedToLoop(); handoverClosesAndStarts(); handoverSplitKeepsUnity(); handoverTailZeroKeepsBaseSmooth(); handoverAtPunchOutKeepsUnity();
    onOffRampsAndResumesInPlace(); volumeIsPlaybackOnly(); softClipOnlyAboveKnee(); dryThruAndBypass();
    clearAllResetsAndOverwrites(); recordOnEmptyTrackZeroesReusedBuffer(); baseUndoRedoAndLengthReset();
    offTrackStillRecords(); disabledEngineOutputsDryOnly();
    stressRandomPresses(0.0f, false); stressRandomPresses(3.0f, true);   // 3 ms: back-dated starts hit a full pool
    REPORT();
}
