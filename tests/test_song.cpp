#include "looper_rig.hpp"
using ouro::TrackState;

// The song: 16 steps of part x repeats, ending in Loop or Stop, advanced at the part ends.

// A: a 24000-sample track (220 Hz); B: its own (880 Hz). Back in A, fed up to A's next part end: A plays at position 0.
static void twoParts(Rig& r) {
    recordBase(r, 24000, 0.1f);
    r.lp.setPart(1); r.silence(24000); r.lp.clearTrack(0); r.silence(4800);
    recordBaseOn(r, 0, 0.1f, 880.0);
    r.lp.setPart(0); r.silence(24000 - r.lp.position());                 // the switch back, at B's end
    r.silence(24000);                                                    // A's next part end
}

static void songPlaysItsStepsInOrder() {
    Rig r; twoParts(r);
    r.lp.setStep(0, 0, 2); r.lp.setStep(1, 1, 1); r.lp.setStep(2, ouro::kStepLoop, 1);
    r.lp.setSong(true); CHECK(r.lp.songOn() && r.lp.songStep() == -1);
    r.silence(24000 + 64); CHECK(r.lp.songStep() == 0 && r.lp.part() == 0);   // starts at the next part end
    r.silence(2 * 24000); CHECK(r.lp.songStep() == 1 && r.lp.part() == 1);
    r.silence(24000); CHECK(r.lp.songStep() == 0 && r.lp.part() == 0);        // Loop
    CHECK(r.lp.songLength() == 2);
}
static void aStopStepStopsAndRewinds() {
    Rig r; twoParts(r); r.lp.setStep(0, 1, 1); r.lp.setStep(1, ouro::kStepStop, 1);
    r.lp.setSong(true); r.silence(24000 + 24000 + 128);
    CHECK(r.lp.transport() == ouro::Transport::Stopped && r.lp.songOn() && r.lp.songStep() == -1);
    r.lp.pressRec(0);                                                    // B from stopped: a restart
    CHECK(r.lp.transport() == ouro::Transport::Running && r.lp.songStep() == 0 && r.lp.part() == 1);
}
static void songFromNothingCountsFromTheFirstClose() {
    Rig r; r.lp.setStep(0, 0, 2); r.lp.setStep(1, 1, 1); r.lp.setSong(true);
    r.lp.pressRec(0); r.feed(sine(220, 0.1f, 24000)); r.lp.pressRec(0);   // closes at position 0
    CHECK(r.lp.songStep() == 0 && r.lp.songPass() == 0);
    r.silence(2 * 24000 + 64);
    CHECK(r.lp.part() == 1 && r.lp.state(0) == TrackState::Playing);    // B filled from A on entry
}
static void stepsAfterTheFirstLoopAreNotPlayed() {
    Rig r; twoParts(r); r.lp.setStep(0, 0, 1); r.lp.setStep(1, ouro::kStepLoop, 1); r.lp.setStep(2, 1, 1);
    r.lp.setSong(true); r.silence(5 * 24000); CHECK(r.lp.part() == 0 && r.lp.songLength() == 1);
}
static void aHandOnPartTurnsTheSongOff() {
    Rig r; twoParts(r); r.lp.setSong(true); r.silence(24000);
    r.lp.setPart(1); CHECK(!r.lp.songOn() && r.lp.queuedPart() == 1 && r.lp.songStep() == -1);
}
static void repeatsCountPassesOfTheWholePart() {
    Rig r; twoParts(r);
    r.lp.pressRec(1); r.feed(sine(330, 0.1f, 2 * 24000)); r.lp.pressRec(1);   // A gets a 2-loop track
    r.silence(2 * 24000 - r.lp.trackPosition(1));                         // A's top
    r.lp.setStep(0, 0, 2); r.lp.setStep(1, 1, 1); r.lp.setSong(true);
    r.silence(2 * 24000 + 64); CHECK(r.lp.songStep() == 0);              // the song starts at A's end
    r.silence(4 * 24000 - 128); CHECK(r.lp.part() == 0);                 // two passes of 2 loops
    r.silence(128); CHECK(r.lp.part() == 1);
}
static void clearAllRewindsTheSong() {
    Rig r; twoParts(r); r.lp.setSong(true); r.silence(2 * 24000);
    r.lp.clearAll(); r.silence(4800);
    CHECK(r.lp.songOn() && r.lp.songStep() == -1 && r.lp.part() == 0);
}
static void theDefaultSongRepeatsA() {
    Rig r; twoParts(r); r.lp.setSong(true); r.silence(5 * 24000);
    CHECK(r.lp.part() == 0 && r.lp.songStep() == 0 && r.lp.songLength() == 1);
}

// R1: Song on with nothing recorded goes to step 1's part at once, so the first loop is recorded there.
static void songOnWithNothingRecordedMovesToStepOnesPart() {
    Rig r; r.lp.setStep(0, 2, 1); r.lp.setSong(true); CHECK(r.lp.part() == 2);
    r.lp.pressRec(0); r.feed(sine(220, 0.1f, 24000)); r.lp.pressRec(0);
    CHECK(r.lp.part() == 2 && r.lp.state(0) == TrackState::Playing && r.lp.songStep() == 0);
    Rig s; s.lp.setSong(true); s.lp.setStep(0, 3, 1); CHECK(s.lp.part() == 3);   // either order (a pedalboard load)
}
// Beyond the brief: with Song on, emptied (Clear All, or every track cleared) means from nothing: step 1's part.
static void emptiedWithSongOnGoesToStepOnesPart() {
    Rig r; twoParts(r); r.lp.setStep(0, 1, 1); r.lp.setSong(true); r.silence(24000 + 64);
    r.lp.setStep(0, 2, 1); r.lp.clearAll(); r.silence(4800);
    CHECK(r.lp.loopLength() == 0 && r.lp.part() == 2 && r.lp.songOn());
    r.lp.setStep(0, 3, 1); CHECK(r.lp.part() == 3);
    r.lp.pressRec(0); r.feed(sine(220, 0.1f, 24000)); r.lp.pressRec(0); r.lp.setStep(0, 1, 1);
    r.lp.clearTrack(0); r.silence(4800);
    CHECK(r.lp.loopLength() == 0 && r.lp.part() == 1);
    r.lp.pressRec(0); r.feed(sine(220, 0.1f, 12000)); r.lp.clearAll();   // during the first recording: at once
    CHECK(r.lp.part() == 1 && r.lp.state(0) == TrackState::Empty);
}
// Beyond the brief: each entry into a step counts its passes from 0, a Loop back to it included.
static void aLoopBackCountsPassesAfresh() {
    Rig r; twoParts(r); r.lp.setStep(0, 0, 2); r.lp.setSong(true);
    r.silence(24000 + 64); CHECK(r.lp.songStep() == 0 && r.lp.songPass() == 0);
    r.silence(24000); CHECK(r.lp.songPass() == 1);
    r.silence(24000); CHECK(r.lp.songStep() == 0 && r.lp.songPass() == 0);   // Loop: step 1 again
}
// Beyond the brief: Song on drives the part: a switch queued by hand goes.
static void songOnCancelsAQueuedSwitch() {
    Rig r; twoParts(r); r.lp.setPart(1); r.lp.setSong(true); CHECK(r.lp.queuedPart() == -1);
    r.silence(24000 + 64); CHECK(r.lp.part() == 0 && r.lp.songStep() == 0);
}
// Beyond the brief: a stop rewinds the song, its part step 1's (R10); the restart plays it from the top.
static void aRestartPlaysTheSongFromTheTop() {
    Rig r; twoParts(r); r.lp.setStep(1, 1, 1); r.lp.setSong(true);
    r.silence(2 * 24000 + 64); CHECK(r.lp.songStep() == 1 && r.lp.part() == 1);
    r.lp.stop(ouro::StopMode::Instant); r.silence(2000);
    CHECK(r.lp.transport() == ouro::Transport::Stopped && r.lp.songStep() == -1 && r.lp.part() == 0);
    r.lp.restart(); CHECK(r.lp.part() == 0 && r.lp.songStep() == 0 && r.lp.songPass() == 0);
    r.silence(64); CHECK(r.lp.trackPosition(0) == 64);
}
// R10: Song on and stopped, the engine sits in step 1's part: Song turned on while stopped elsewhere, step 1 edited.
static void stoppedWithSongOnSitsInStepOnesPart() {
    Rig r; twoParts(r); r.lp.setPart(1); r.silence(24000 - r.lp.position() + 1000);
    r.lp.stop(ouro::StopMode::Instant); r.silence(2000);
    CHECK(r.lp.transport() == ouro::Transport::Stopped && r.lp.part() == 1);
    r.lp.setSong(true); CHECK(r.lp.part() == 0);
    r.lp.setStep(0, 1, 1); CHECK(r.lp.part() == 1);
}
// Beyond the brief: MOD's start mid-song plays step 1's part from its top, crossfaded from the part playing.
static void aModStartPlaysTheSongFromTheTop() {
    Rig r; twoParts(r); r.lp.setStep(1, 1, 1); r.lp.setSong(true);
    r.silence(2 * 24000 + 6000); CHECK(r.lp.songStep() == 1 && r.lp.part() == 1);
    const size_t from = r.out.size() - 1;
    r.lp.modStarted(true); r.silence(64);
    CHECK(r.lp.part() == 0 && r.lp.songStep() == 0 && r.lp.position() == 64 && r.lp.trackPosition(0) == 64);
    r.silence(4800);
    CHECK(maxStep(slice(r.out, from, r.out.size())) <= 1.5f * (sineStep(880.0, 0.1f) + sineStep(220.0, 0.1f)));
}
// Beyond the brief: a Stop step uses the Stop Mode. End of loop ends on the part end as Instant does; Fade fades out
// from there over Fade Time.
static void aStopStepUsesTheStopMode() {
    Rig e; twoParts(e); e.lp.setStopMode(ouro::StopMode::EndOfLoop, 3000.0f);
    e.lp.setStep(1, ouro::kStepStop, 1); e.lp.setSong(true); e.silence(2 * 24000 + 128);
    CHECK(e.lp.transport() == ouro::Transport::Stopped && e.lp.songStep() == -1);
    Rig f; twoParts(f); f.lp.setStopMode(ouro::StopMode::Fade, 1000.0f);
    f.lp.setStep(1, ouro::kStepStop, 1); f.lp.setSong(true); f.silence(2 * 24000 + 128);
    CHECK(f.lp.transport() == ouro::Transport::Stopping && f.lp.songStep() == 0);
    f.silence(48000); CHECK(f.lp.transport() == ouro::Transport::Stopped && f.lp.songStep() == -1);
}

// R10 (T3 M1): the song ends where its steps now end. B, playing, is edited to Stop: the loops stop at B's end.
static void editingThePlayingStepToStopStopsAtItsEnd() {
    Rig r; twoParts(r);
    r.lp.setStep(1, 1, 1); r.lp.setStep(2, 0, 2); r.lp.setStep(3, ouro::kStepLoop, 1);   // A, B, A x2, Loop
    r.lp.setSong(true); r.silence(2 * 24000 + 64); CHECK(r.lp.songStep() == 1 && r.lp.part() == 1);
    r.lp.setStep(1, ouro::kStepStop, 1);
    r.silence(24000 - 64 + 128);
    CHECK(r.lp.transport() == ouro::Transport::Stopped && r.lp.songStep() == -1);
}
// R10 (T3 M2): B cancelling the song's Stop (End of loop, Fade) rewinds the song: step 1 starts at the next part end.
static void cancellingTheSongsStopRewindsIt() {
    for (ouro::StopMode m : {ouro::StopMode::EndOfLoop, ouro::StopMode::Fade}) {
        Rig r; twoParts(r); r.lp.setStopMode(m, 1000.0f);
        r.lp.setStep(1, 1, 1); r.lp.setStep(2, ouro::kStepStop, 1);    // A, B, Stop
        r.lp.setSong(true);
        const uint32_t at = m == ouro::StopMode::Fade ? 3 * 24000 + 2000 : 3 * 24000 - 360;   // in the stop's fade
        r.silence(at); CHECK(r.lp.transport() == ouro::Transport::Stopping);
        r.lp.pressRec(0); CHECK(r.lp.transport() == ouro::Transport::Running);
        r.silence((at / 24000 + 1) * 24000 - at + 64);                 // the next part end
        CHECK(r.lp.transport() == ouro::Transport::Running && r.lp.part() == 0 && r.lp.songStep() == 0);
        r.silence(24000); CHECK(r.lp.part() == 1 && r.lp.songStep() == 1);
    }
}

int main() {
    editingThePlayingStepToStopStopsAtItsEnd(); cancellingTheSongsStopRewindsIt();
    songPlaysItsStepsInOrder(); aStopStepStopsAndRewinds(); songFromNothingCountsFromTheFirstClose();
    stepsAfterTheFirstLoopAreNotPlayed(); aHandOnPartTurnsTheSongOff(); repeatsCountPassesOfTheWholePart();
    clearAllRewindsTheSong(); theDefaultSongRepeatsA(); songOnWithNothingRecordedMovesToStepOnesPart();
    emptiedWithSongOnGoesToStepOnesPart(); aLoopBackCountsPassesAfresh(); aRestartPlaysTheSongFromTheTop();
    aModStartPlaysTheSongFromTheTop(); aStopStepUsesTheStopMode(); songOnCancelsAQueuedSwitch();
    stoppedWithSongOnSitsInStepOnesPart();
    REPORT();
}
