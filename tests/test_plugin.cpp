#include "OuroborosPlugin.hpp"
#include "src/DistrhoPluginInternal.hpp"
#include "signals.hpp"
#include "test_util.hpp"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <sys/stat.h>

USE_NAMESPACE_DISTRHO

struct Req { uint32_t index; float value; };
static std::vector<Req> gReqs;

static bool requestStub(void*, uint32_t index, float value)
{
    gReqs.push_back({index, value});
    return true;
}
static bool hasReq(uint32_t index, float value)
{
    for (const Req& r : gReqs)
        if (r.index == index && r.value == value) return true;
    return false;
}

static std::unique_ptr<PluginExporter> gPlugin;
static PluginExporter& P() { return *gPlugin; }
static void fresh() { gPlugin.reset(new PluginExporter(nullptr, nullptr, requestStub, nullptr)); gReqs.clear(); }
static void set(uint32_t i, float v) { P().setParameterValue(i, v); }
static OuroborosPlugin& plug() { return *static_cast<OuroborosPlugin*>(P().getInstancePointer()); }
static const ouro::Looper& eng() { return plug().engine(); }

// Process vec in 128-frame blocks into gOut. gOut is prefilled with a nonzero
// sentinel so a run() that never writes its output fails the silence checks.
static std::vector<float> gOut, gClick;                    // the loop output and the Click output
static void run(PluginExporter& plugin, const std::vector<float>& vec)
{
    gOut.assign(vec.size(), 0.777f);
    gClick.assign(vec.size(), 0.777f);
    for (size_t i = 0; i < vec.size(); i += 128)
    {
        const uint32_t len = static_cast<uint32_t>(std::min<size_t>(128, vec.size() - i));
        const float* inputs[1]  = { vec.data() + i };
        float*       outputs[2] = { gOut.data() + i, gClick.data() + i };
        plugin.run(inputs, outputs, len);
    }
}
static void runN(int k) { run(P(), sine(220, 0.1f, 128u * k)); }

// Base of 24064 samples on track 0, then a 4800-sample overdub, via kParamRec presses.
static void recordBaseAndDub()
{
    set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(187);
    set(kParamRec, 1); runN(1); set(kParamRec, 0);                  // base closed: 188 blocks
    runN(40);                                                       // let the 50 ms tail finish
    set(kParamRec, 1); runN(1); set(kParamRec, 0);                  // overdub starts
    run(P(), sine(220, 0.1f, 4800 - 128));                          // 4800 samples in all
    set(kParamRec, 1); runN(1); set(kParamRec, 0);                  // overdub closed
    runN(40);                                                       // let the tail and fade finish
}

static void layout()
{
    fresh();
    static const char* kSym[] = { "rec","stop_clear","track","onoff","volume","undo","redo","clear_track",
        "clear_all","on_1","on_2","on_3","on_4","volume_1","volume_2","volume_3","volume_4","after_rec",
        "dry","tail","hold_time","budget","offset","trigger_latency",
        "stop_play","stop_mode","fade_time","auto_rec","threshold","double_tap","sync","click","click_level","length",
        "part","song","step","step_part","step_repeats" };
    for (uint32_t i = 0; i < 39; ++i) CHECK(std::strcmp(P().getParameterSymbol(i), kSym[i]) == 0);
    for (uint32_t s = 0; s < 16; ++s)                                   // step1_part, step1_repeats, ... hidden
        for (uint32_t k = 0; k < 2; ++k)
        {
            const uint32_t i = kParamStep1Part + 2 * s + k;
            const std::string sym = "step" + std::to_string(s + 1) + (k ? "_repeats" : "_part");
            CHECK(std::strcmp(P().getParameterSymbol(i), sym.c_str()) == 0);
            CHECK((P().getParameterHints(i) & kParameterIsHidden) != 0 && P().getParameterGroupId(i) == 3);
            CHECK(P().getParameterRanges(i).def == (k ? 1.0f : s == 0 ? 0.0f : 4.0f));
        }
    for (uint32_t i = kParamPart; i <= kParamStepRepeats; ++i)
        CHECK((P().getParameterHints(i) & kParameterIsHidden) == 0 && P().getParameterGroupId(i) == 3);
    CHECK(std::strcmp(P().getPortGroupById(3).symbol, "g4_song") == 0);
    CHECK(P().getParameterEnumValues(kParamPart).count == 4 && P().getParameterEnumValues(kParamStepPart).count == 6);
    CHECK(std::strcmp(P().getParameterEnumValues(kParamStepPart).values[5].label, "Stop") == 0);
    CHECK(P().getParameterRanges(kParamStep).min == 1.0f && P().getParameterRanges(kParamStep).max == 16.0f);
    CHECK(P().getParameterRanges(kParamStepRepeats).max == 16.0f && P().getParameterRanges(kParamSong).def == 0.0f);
    CHECK(P().getParameterDesignation(kParamBypass) == kParameterDesignationBypass);
    CHECK(kParamBypass == 75 && P().getParameterCount() == 76);
    static const char* kPieceSym[] = { "slot", "save", "load", "piece" };   // before Bypass, in g5_pieces
    for (uint32_t i = 0; i < 4; ++i)
        CHECK(std::strcmp(P().getParameterSymbol(kParamSlot + i), kPieceSym[i]) == 0 && P().getParameterGroupId(kParamSlot + i) == 4);
    CHECK(std::strcmp(P().getPortGroupById(4).symbol, "g5_pieces") == 0);
    CHECK((P().getParameterHints(kParamPiece) & kParameterIsHidden) != 0 && (P().getParameterHints(kParamSlot) & kParameterIsHidden) == 0);
    CHECK(P().isParameterTrigger(kParamSave) && P().isParameterTrigger(kParamLoad) && !P().isParameterTrigger(kParamSlot));
    CHECK(P().getParameterRanges(kParamSlot).min == 1.0f && P().getParameterRanges(kParamSlot).max == 16.0f);
    CHECK(P().getParameterRanges(kParamSlot).def == 1.0f && P().getParameterRanges(kParamPiece).def == 0.0f);
    CHECK(P().getParameterRanges(kParamPiece).min == 0.0f && P().getParameterRanges(kParamPiece).max == 16.0f);
    CHECK(std::strcmp(P().getLabel(), DISTRHO_PLUGIN_NAME) == 0);
    // groups: g1_perform 0-8, g2_tracks 9-16, g3_settings 17-23,25-33; stop_play (24) in perform
    for (uint32_t i = 0; i < 34; ++i)
    {
        const uint32_t g = P().getParameterGroupId(i);
        CHECK(g == (i <= 8 || i == 24 ? 0u : i <= 16 ? 1u : 2u));
    }
    CHECK(std::strcmp(P().getPortGroupById(0).symbol, "g1_perform") == 0);
    CHECK(std::strcmp(P().getPortGroupById(1).symbol, "g2_tracks") == 0);
    CHECK(std::strcmp(P().getPortGroupById(2).symbol, "g3_settings") == 0);
    CHECK(!P().isParameterTrigger(kParamRec) && !P().isParameterTrigger(kParamStopClear));
    CHECK(P().getParameterRanges(kParamVolume).def == 0.0f && P().getParameterRanges(kParamVolume).min == -60.0f);
    CHECK(P().getParameterRanges(kParamTail).def == 50.0f && P().getParameterRanges(kParamHoldTime).def == 500.0f);
    CHECK(P().getParameterRanges(kParamBudget).min == 16.0f && P().getParameterRanges(kParamBudget).def == 200.0f);
    CHECK(P().getParameterRanges(kParamTriggerLatency).def == 3.0f && P().getParameterRanges(kParamOn1).def == 1.0f);
    CHECK(P().getParameterEnumValues(kParamStopMode).count == 3 && P().getParameterRanges(kParamFadeTime).def == 3.0f);
    CHECK(P().getParameterRanges(kParamThreshold).def == -40.0f && P().getParameterRanges(kParamDoubleTap).def == 400.0f && P().getParameterRanges(kParamAutoRec).def == 0.0f);
    CHECK(P().getParameterEnumValues(kParamSync).count == 2 && P().getParameterRanges(kParamSync).def == 0.0f);
    CHECK(P().getParameterRanges(kParamClick).def == 0.0f && P().getParameterRanges(kParamClickLevel).def == -10.0f);
    CHECK(P().getParameterRanges(kParamClickLevel).min == -40.0f && P().getParameterRanges(kParamClickLevel).max == 0.0f);
    CHECK(P().getParameterEnumValues(kParamLength).count == 17 && P().getParameterRanges(kParamLength).def == 0.0f);
    CHECK(std::strcmp(P().getParameterEnumValues(kParamLength).values[0].label, "Free") == 0);
    CHECK(std::strcmp(P().getParameterEnumValues(kParamLength).values[16].label, "16 bars") == 0);
    CHECK(P().getParameterRanges(kParamDry).min == -60.0f && P().getParameterRanges(kParamDry).max == 6.0f);
    CHECK(P().getParameterRanges(kParamDry).def == -60.0f && !P().isParameterInteger(kParamDry));
    for (const uint32_t i : {uint32_t(kParamStep), uint32_t(kParamSlot)})   // lists on MOD, not bars
    {
        const ParameterEnumerationValues& e = P().getParameterEnumValues(i);
        CHECK(e.count == 16 && e.restrictedMode && P().getParameterRanges(i).def == 1.0f);
        CHECK(e.values[0].value == 1.0f && std::strcmp(e.values[0].label, "1") == 0);
        CHECK(e.values[15].value == 16.0f && std::strcmp(e.values[15].label, "16") == 0);
    }
}

static void silentAndMono()
{
    fresh();
    CHECK(DISTRHO_PLUGIN_NUM_INPUTS == 1 && DISTRHO_PLUGIN_NUM_OUTPUTS == 2);   // the loops, then the click
    CHECK(std::strcmp(P().getAudioPort(false, 1).symbol, "click_out") == 0 && P().getAudioPort(false, 1).groupId == kPortGroupNone);
    CHECK(std::strcmp(P().getAudioPort(false, 0).symbol, "audio_out_1") == 0 && P().getAudioPort(false, 0).groupId == kPortGroupMono);
    P().activate();
    run(P(), sine(220, 0.1f, 4800));        // nothing recorded, Dry off -> silence
    CHECK(peak(gOut) == 0.0f && peak(gClick) == 0.0f);
}

static void recActsOnRisingEdgeOnly()
{
    fresh();
    set(kParamRec, 1); runN(1); CHECK(eng().state(0) == ouro::TrackState::Recording);
    set(kParamRec, 1); runN(3); CHECK(eng().state(0) == ouro::TrackState::Recording);   // held high: no new edge
    set(kParamRec, 0); runN(188); CHECK(eng().state(0) == ouro::TrackState::Recording);
    set(kParamRec, 1); runN(1);   CHECK(eng().state(0) == ouro::TrackState::Playing);
}

static void bDown() { set(kParamRec, 1); runN(1); }
static void bUp()   { set(kParamRec, 0); runN(1); }
static void cDown() { set(kParamStopClear, 1); runN(1); }
static void cUp()   { set(kParamStopClear, 0); runN(1); }
static void tapC() { cDown(); runN(19); cUp(); }
static void doubleTap() { tapC(); runN(29); set(kParamStopClear, 1); runN(1); set(kParamStopClear, 0); runN(1); }

static void cPressMutesAndReleaseUnmutes() {
    fresh(); recordBaseAndDub();
    cDown(); CHECK(!eng().isOn(0) && hasReq(kParamOn1, 0.0f)); runN(19); cUp(); runN(160);
    CHECK(!eng().isOn(0));                                              // stays off
    cDown(); CHECK(!eng().isOn(0)); runN(19); cUp(); CHECK(eng().isOn(0)); // on at the release
}
static void cHoldClearsTheTrackAndSetsItOn() {
    fresh(); recordBaseAndDub();
    cDown(); runN(186); CHECK(!eng().isOn(0) && eng().state(0) == ouro::TrackState::Playing);
    runN(2); CHECK(eng().state(0) == ouro::TrackState::Empty && eng().isOn(0)); cUp();
}
static void cHoldOnAnOffTrackNeverSounds() {
    fresh(); recordBaseAndDub(); tapC(); runN(160); CHECK(!eng().isOn(0));
    cDown(); for (int i = 0; i < 186; ++i) { runN(1); CHECK(!eng().isOn(0)); }
    runN(2); CHECK(eng().state(0) == ouro::TrackState::Empty && eng().isOn(0)); cUp();
}
static void cDoubleTapRestoresThenStops() {
    fresh(); recordBaseAndDub(); tapC(); runN(29); cDown();
    CHECK(eng().isOn(0) && eng().transport() != ouro::Transport::Running); cUp();
}
static void cDoubleTapPutsBackTheOnBeforeTheTap() {                     // not a flip: On/Off between the taps
    fresh(); recordBaseAndDub(); tapC(); set(kParamOnOff, 1); runN(1); set(kParamOnOff, 0); runN(1); CHECK(eng().isOn(0));
    cDown(); CHECK(eng().isOn(0)); cUp();
}
static void cDoubleTapRestoresBeforeAFadeStop() {                       // Review Focus 3
    fresh(); set(kParamStopMode, 2); recordBaseAndDub(); tapC(); runN(29); cDown();
    CHECK(eng().isOn(0) && eng().transport() == ouro::Transport::Stopping); cUp();
}
static void cTapOnAnEmptyTrackDoesNothing() {
    fresh(); recordBaseAndDub(); set(kParamTrack, 2); runN(1);
    tapC(); runN(160); CHECK(eng().isOn(1));
    tapC(); runN(29); cDown(); CHECK(eng().transport() != ouro::Transport::Running); cUp();  // a double-tap still stops
}
static void cDuringAnOverdubClosesItAndMutes() {
    fresh(); recordBaseAndDub(); set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(60);
    cDown(); CHECK(eng().state(0) == ouro::TrackState::Playing && eng().undoDepth(0) == 2 && !eng().isOn(0)); cUp();
}
static void cTapDuringATakeOnAnOffTrackTurnsItOn() {                    // the label read OFF n
    fresh(); recordBaseAndDub(); set(kParamOn1, 0); runN(1); bDown(); bUp(); runN(20);   // an overdub, track 1 off
    CHECK(eng().state(0) == ouro::TrackState::Overdubbing && !eng().isOn(0));
    gReqs.clear(); tapC(); CHECK(eng().state(0) == ouro::TrackState::Playing && eng().isOn(0) && hasReq(kParamOn1, 1.0f));
    runN(160); set(kParamTrack, 2); set(kParamOn2, 0); runN(1); bDown(); bUp(); runN(20);   // a recording onto the loop
    CHECK(eng().state(1) == ouro::TrackState::Recording && !eng().isOn(1));
    tapC(); CHECK(eng().state(1) == ouro::TrackState::Playing && eng().isOn(1));
}
static void cCancelsOnlyTheSelectedTracksArm() {                        // runN's 0.1 sine stays under -10 dB
    fresh(); recordBaseAndDub(); set(kParamAutoRec, 1); set(kParamThreshold, -10); set(kParamTrack, 3); runN(1);
    set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(1); CHECK(eng().armedTrack() == 2);
    set(kParamTrack, 1); runN(1); tapC(); CHECK(eng().armedTrack() == 2 && !eng().isOn(0));
    runN(160); set(kParamTrack, 3); runN(1); tapC(); CHECK(eng().armedTrack() == -1 && eng().isOn(2));
}
static void cDuringTheFirstRecording() {
    fresh(); set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(100);
    tapC(); CHECK(eng().state(0) == ouro::TrackState::Recording);       // the tap waits out the window
    runN(160); CHECK(eng().state(0) == ouro::TrackState::Empty);
    fresh(); set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(100);
    cDown(); runN(188); CHECK(eng().state(0) == ouro::TrackState::Empty); cUp();   // a hold aborts it
    fresh(); set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(100);
    tapC(); runN(10); set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(1);    // B while the abort is pending
    CHECK(eng().state(0) == ouro::TrackState::Recording && eng().loopLength() == 0);
}
static void cTapPendingDoesNotWipeALoopBClosed() {                     // Ruling R6
    fresh(); bDown(); bUp(); runN(100);                                 // the first recording
    cDown(); runN(5); bDown(); bUp(); runN(5); cUp();                   // B closes it while C is held, then a tap
    runN(160); CHECK(eng().loopLength() > 0 && eng().state(0) == ouro::TrackState::Playing);
    fresh(); bDown(); bUp(); runN(100);
    cDown(); runN(5); bDown(); bUp(); runN(5); cUp(); runN(10);         // the same, then B within the window
    bDown(); bUp(); CHECK(eng().loopLength() > 0 && eng().state(0) == ouro::TrackState::Overdubbing);
}
static void doubleTapStops() {
    fresh(); recordBaseAndDub(); tapC(); runN(29);
    set(kParamStopClear, 1); runN(1); CHECK(eng().transport() == ouro::Transport::Stopping);   // on the press
    runN(20); set(kParamStopClear, 0); runN(200);
    CHECK(eng().transport() == ouro::Transport::Stopped && eng().isOn(0) && peak(gOut) == 0.0f);
}
static void bRestartsAfterStop() {
    fresh(); recordBaseAndDub(); doubleTap(); runN(20);
    set(kParamRec, 1); runN(1); set(kParamRec, 0);
    CHECK(eng().transport() == ouro::Transport::Running && eng().state(0) == ouro::TrackState::Playing);
}
static void doubleTapThenHoldDoesNotClear() {                          // a second press is never a first press
    fresh(); recordBaseAndDub(); tapC(); runN(29); set(kParamStopClear, 1); runN(450);
    CHECK(eng().transport() == ouro::Transport::Stopped && eng().loopLength() > 0);
    CHECK(eng().state(0) == ouro::TrackState::Playing && eng().isOn(0)); cUp();
}
static void doubleTapBegunWhileStoppingOnlyStops() {                   // first tap during a fade, second after the park
    fresh(); recordBaseAndDub(); set(kParamStopMode, 2); set(kParamFadeTime, 0.5f);
    set(kParamStopPlay, 1); runN(1); set(kParamStopPlay, 0); runN(170);   // 171 blocks into a 188-block fade
    set(kParamStopClear, 1); runN(2); set(kParamStopClear, 0); runN(1);    // released while still stopping
    CHECK(eng().transport() == ouro::Transport::Stopping && !eng().isOn(0));   // as while running: off
    runN(20); CHECK(eng().transport() == ouro::Transport::Stopped);
    set(kParamStopClear, 1); runN(1); set(kParamStopClear, 0); runN(120);
    CHECK(eng().transport() == ouro::Transport::Stopped && eng().isOn(0));   // put back, still stopped
}
static void doubleTapDuringAShortFirstRecording() {                   // Review Focus 4
    fresh(); set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(5);
    set(kParamStopClear, 1); runN(2); set(kParamStopClear, 0); runN(2); set(kParamStopClear, 1); runN(1);
    CHECK(eng().state(0) == ouro::TrackState::Empty && eng().loopLength() == 0
          && eng().transport() == ouro::Transport::Running);
    set(kParamStopClear, 0); runN(200); CHECK(eng().state(0) == ouro::TrackState::Empty);
    fresh(); bDown(); bUp(); runN(100);                                 // a long enough take: kept as the loop
    tapC(); runN(29); cDown(); cUp(); runN(200);
    CHECK(eng().loopLength() > 0 && eng().transport() == ouro::Transport::Stopped);
}
static void stopPlayPortToggles() {
    fresh(); recordBaseAndDub();
    set(kParamStopPlay, 1); runN(1); set(kParamStopPlay, 0); CHECK(eng().transport() == ouro::Transport::Stopping);
    runN(20); CHECK(eng().transport() == ouro::Transport::Stopped);
    set(kParamStopPlay, 1); runN(1); set(kParamStopPlay, 0); CHECK(eng().transport() == ouro::Transport::Running);
}
static void stopAndAutoRecSettingsReachTheEngine() {
    fresh(); recordBaseAndDub(); set(kParamStopMode, 2); set(kParamFadeTime, 0.5f);
    set(kParamStopPlay, 1); runN(1); set(kParamStopPlay, 0);
    runN(90);  CHECK(eng().transport() == ouro::Transport::Stopping);   // 240 ms into a 500 ms fade
    runN(110); CHECK(eng().transport() == ouro::Transport::Stopped);
    set(kParamStopPlay, 1); runN(1); set(kParamStopPlay, 0);
    set(kParamAutoRec, 1); set(kParamThreshold, -10); set(kParamTrack, 2); runN(1);
    set(kParamRec, 1); runN(5); set(kParamRec, 0);                       // runN's 0.1 sine stays under -10 dB
    CHECK(eng().armedTrack() == 1 && eng().state(1) == ouro::TrackState::Empty);
    set(kParamThreshold, -40); runN(1); CHECK(eng().state(1) == ouro::TrackState::Recording);
}

static void undoRedoClearTriggers()
{
    fresh();
    recordBaseAndDub();
    set(kParamUndo, 1); runN(1); set(kParamUndo, 0); runN(1);
    CHECK(eng().undoDepth(0) == 0 && eng().canRedo(0));
    set(kParamRedo, 1); runN(1); set(kParamRedo, 0); runN(1);
    CHECK(eng().undoDepth(0) == 1);
    set(kParamClearTrack, 1); runN(1); set(kParamClearTrack, 0); runN(1);
    CHECK(eng().state(0) == ouro::TrackState::Empty);
    set(kParamTrack, 2); runN(1);
    set(kParamRec, 1); runN(1); set(kParamRec, 0);
    set(kParamTrack, 1); runN(1);
    set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(10);
    set(kParamClearAll, 1); runN(1); set(kParamClearAll, 0); runN(1);
    CHECK(eng().state(0) == ouro::TrackState::Empty && eng().state(1) == ouro::TrackState::Empty);
}

static void trackSelectRetargets()
{
    fresh();
    set(kParamTrack, 2); runN(1); set(kParamRec, 1); runN(1);
    CHECK(eng().state(1) == ouro::TrackState::Recording);
}

static void volumeProxyFollowsSelectedTrack()
{
    fresh();
    set(kParamVolume2, -12); runN(1); gReqs.clear();
    set(kParamTrack, 2); runN(1); CHECK(hasReq(kParamVolume, -12.0f));
    set(kParamVolume, -10); runN(1);
    CHECK(P().getParameterValue(kParamVolume2) == -10.0f && eng().volumeDb(1) == -10.0f);
    CHECK(hasReq(kParamVolume2, -10.0f));
    // the host echoes the requested value back: a no-op, no new request
    gReqs.clear();
    set(kParamVolume2, -10); set(kParamVolume, -10); runN(2);
    CHECK(gReqs.empty() && eng().volumeDb(1) == -10.0f);
    // a per-track change on the selected track moves the proxy; on another track it does not
    set(kParamVolume2, -20); runN(1); CHECK(hasReq(kParamVolume, -20.0f));
    gReqs.clear(); set(kParamVolume3, -30); runN(1);
    CHECK(eng().volumeDb(2) == -30.0f && !hasReq(kParamVolume, -30.0f) && gReqs.empty());
}

static void onoffFlipsSelectedAndReportsPort()
{
    fresh();
    set(kParamTrack, 3); runN(1); gReqs.clear(); set(kParamOnOff, 1); runN(1);
    CHECK(!eng().isOn(2) && hasReq(kParamOn3, 0.0f));
    set(kParamOnOff, 0); runN(1); gReqs.clear(); set(kParamOnOff, 1); runN(1);
    CHECK(eng().isOn(2) && hasReq(kParamOn3, 1.0f));
    set(kParamOn1, 0); runN(1); CHECK(!eng().isOn(0));      // per-track port drives the engine directly
}

static void settingsReachTheEngine()
{
    fresh();
    set(kParamAfterRec, 1);
    set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(10);
    CHECK(eng().nextAction(0) == ouro::NextAction::Dub);    // Rec > Dub
    set(kParamBudget, 16); set(kParamTail, 100); set(kParamOffset, 5); set(kParamTriggerLatency, 10); runN(1);
}

static void bypassSemantics()      // replaces the template's test_bypass
{
    // 240 Hz at 48 kHz is exactly 200 samples per period, so a 24000-sample loop wraps smoothly.
    const auto s = sine(240, 0.1f, 48000);
    const float limit = 2.5f * sineStep(240, 0.1f);
    for (int dry = 0; dry < 2; ++dry)                           // Dry off, then at -6 dB: bypass passes it at that level
    {
        fresh();
        set(kParamDry, dry ? -6.0f : -60.0f);
        const float g = dry ? ouro::dryGain(-6.0f) : 0.0f;
        size_t pos = 0;
        std::vector<float> out;
        auto feed = [&](size_t n) {
            run(P(), slice(s, pos, pos + n));
            out.insert(out.end(), gOut.begin(), gOut.end());
            pos += n;
        };
        set(kParamRec, 1); feed(128); set(kParamRec, 0); feed(24000 - 128);
        set(kParamRec, 1); feed(128); set(kParamRec, 0);         // loop closed: playing
        feed(9600);
        CHECK(eng().state(0) == ouro::TrackState::Playing);
        const size_t from = out.size();
        set(kParamBypass, 1); feed(4800);
        const size_t settled = out.size() - 720;                 // 15 ms after the toggle
        for (size_t i = settled; i < out.size(); ++i)
        {
            const float want = g * s[pos - (out.size() - i)];
            CHECK(std::fabs(out[i] - want) < 1e-6f);
        }
        set(kParamBypass, 0); feed(4800);
        CHECK(maxStep(slice(out, from - 1, out.size())) < limit);
        CHECK(peak(slice(out, out.size() - 2400, out.size())) > 0.05f);
    }
}

static void savedValuesSnapOnLoad()                               // Review Focus 2
{
    fresh(); set(kParamDry, 0.0f); P().activate();
    auto s = sine(220, 0.1f, 128); run(P(), s); CHECK(maxAbsDiff(gOut, s) < 1e-7f);   // no ramp from 0
}

static void reactivateResets()                                    // Review Focus 1
{
    fresh();
    P().activate(); recordBaseAndDub(); P().deactivate(); P().activate(); runN(1);
    CHECK(eng().loopLength() == 0 && eng().state(0) == ouro::TrackState::Empty && eng().pendingOps(0) == 0);
    P().deactivate(); P().setSampleRate(96000.0, true); P().activate();
    set(kParamRec, 1); runN(1); CHECK(eng().state(0) == ouro::TrackState::Recording);
}

static void disabledEngineStillPassesDry()                        // Review Focus 4
{
    d_nextSampleRate = 1e12;                                      // the reservation fails
    fresh();
    d_nextSampleRate = 48000.0;
    CHECK(!eng().enabled());
    set(kParamDry, 0.0f); P().activate();
    auto s = sine(220, 0.1f, 512); run(P(), s);
    CHECK(maxAbsDiff(gOut, s) < 1e-7f);
    set(kParamRec, 1); runN(1); CHECK(eng().state(0) == ouro::TrackState::Empty);
    set(kParamDry, -60.0f); P().deactivate(); P().activate(); run(P(), s); CHECK(peak(gOut) == 0.0f);
}

// The loop widget shows each track's layer count from the engine: a count change alone changes the bitmap, so it redraws.
static std::vector<uint8_t> gBits;
static void drawStub(ALABS_HMI_WidgetDraw_Handle, ALABS_HMI_Widget, uint16_t, uint16_t, uint16_t w, uint16_t h, const uint8_t* bits)
{
    gBits.assign(bits, bits + (w + 7) / 8 * h);
}
static bool countRowShows(uint32_t n)   // 40x37: track 1's count at x 1..7, rows 21..25 (0: none)
{
    uint8_t ref[5 * 37] = {}; if (n) ouro::drawCount(ref, 40, 1, 21, n);
    return gBits.size() == sizeof ref && std::memcmp(gBits.data() + 5 * 20, ref + 5 * 20, 5 * 7) == 0;
}
static void widgetShowsLayerCounts()
{
    static ALABS_HMI_WidgetDraw draw = { sizeof(ALABS_HMI_WidgetDraw), nullptr, drawStub };
    static const LV2_Feature feature = { ALABS_HMI__WidgetDraw, &draw };
    static const LV2_Feature* const features[] = { &feature, nullptr };
    d_nextLv2Features = features; fresh(); d_nextLv2Features = nullptr;
    const ALABS_HMI_WidgetArea area = { sizeof(area), 40, 37, ALABS_HMI_PixelFormat_Mono1 };
    static_cast<OuroborosPlugin*>(P().getInstancePointer())->widgetShown("loop", (ALABS_HMI_Widget)0x1, &area);
    runN(8); CHECK(countRowShows(0));                                   // empty: no count
    recordBaseAndDub(); CHECK(eng().layerCount(0) == 2 && countRowShows(2));
    set(kParamUndo, 1); runN(8); set(kParamUndo, 0); runN(1);
    CHECK(eng().layerCount(0) == 1 && countRowShows(1));
}

static double gT = 0.0;                                             // MOD's transport position, in samples
static void runMod(bool playing, int k) {                           // k blocks; the time position advances as MOD's would
    for (int i = 0; i < k; ++i) {
        const double beats = std::fmod(gT / 24000.0, 4.0);          // 120 BPM 4/4 at 48 kHz: 24000 samples a beat
        TimePosition t; t.playing = playing; t.frame = uint64_t(gT); t.bbt.valid = true; t.bbt.bar = 1 + int(gT / 96000.0);
        t.bbt.beat = 1 + int(beats); t.bbt.tick = (beats - int(beats)) * 1920.0; t.bbt.barStartTick = 0;
        t.bbt.beatsPerBar = 4; t.bbt.beatType = 4; t.bbt.ticksPerBeat = 1920; t.bbt.beatsPerMinute = 120;
        P().setTimePosition(t); runN(1);
        if (playing) gT += 128;
    }
}
static void syncPressWaitsForTheBar() {
    fresh(); gT = 50000; set(kParamSync, 1); runMod(true, 1);       // mid-bar
    set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0);
    CHECK(eng().waiting() && eng().state(0) == ouro::TrackState::Empty);
    runMod(true, 358);                                              // past the next bar line (96000)
    CHECK(!eng().waiting() && eng().state(0) == ouro::TrackState::Recording);
}
static void modPlayAndStopEdgesFollow() {
    fresh(); gT = 0; set(kParamSync, 1); runMod(true, 1);           // the first run is no edge
    set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0);           // 128 after the line: back-dated to it
    runMod(true, 748 - 1);                                          // to 96000 - 128
    set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0); runMod(true, 10);
    CHECK(eng().loopLength() == 96000);
    runMod(false, 1); runMod(false, 20); CHECK(eng().transport() == ouro::Transport::Stopped);   // stop edge
    gT = 0; runMod(true, 1); CHECK(eng().transport() == ouro::Transport::Running);               // play edge, on the line
}
static void syncSwitchedOnWhilePlayingIsNoEdge() {
    fresh(); gT = 30000; runMod(true, 1);                           // Sync Off: a free loop
    set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0); runMod(true, 100);
    set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0); runMod(true, 5);
    const uint32_t p = eng().position();
    set(kParamSync, 1); runMod(true, 1);
    CHECK(eng().position() == (p + 128) % eng().loopLength() && eng().transport() == ouro::Transport::Running);
    CHECK(!eng().waiting());                                        // no retrigger queued for the next line either
}
static void pressSeesThisBlocksGrid() {                             // setGrid comes before the presses
    fresh(); gT = 50000; set(kParamSync, 1); runMod(true, 1);
    gT = 97000; set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0);   // MOD jumped to 1000 past a line: in grace
    CHECK(eng().state(0) == ouro::TrackState::Recording);
}
// A C press with the selected track's wait pending cancels it and does nothing else: the first recording the line
// would have closed goes on, not aborted once the window passes. A second press still stops, and B after the tap
// closes again (no abort first).
static void runTo(double t) { while (gT < t) runMod(true, 1); }
static void waitingClose() {                                        // a first recording from the line; B early for bar 3
    fresh(); gT = 0; set(kParamSync, 1); runMod(true, 1);
    set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0);
    runTo(192000 - 40000); set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0);
    CHECK(eng().userWaiting(0));
}
static void cCancelsTheSelectedTracksWait() {
    for (double before : {14400.0, 1280.0}) {                       // 300 ms before the line; 27 ms (released after it)
        waitingClose(); runTo(192000 - before);
        set(kParamStopClear, 1); runMod(true, 1); CHECK(!eng().waiting() && eng().state(0) == ouro::TrackState::Recording);
        runMod(true, 19); set(kParamStopClear, 0); runMod(true, 200);  // a 53 ms tap; the line and the window pass
        CHECK(eng().state(0) == ouro::TrackState::Recording && eng().loopLength() == 0 && !eng().waiting());
        CHECK(eng().recordedSamples() > 192000);                    // still recording
    }
    waitingClose(); runTo(192000 - 14400);                          // a double-tap stops: whole bars kept
    set(kParamStopClear, 1); runMod(true, 2); set(kParamStopClear, 0); runMod(true, 2);
    set(kParamStopClear, 1); runMod(true, 1); set(kParamStopClear, 0); runMod(true, 40);
    CHECK(eng().transport() == ouro::Transport::Stopped && eng().loopLength() == 96000);
    waitingClose(); runTo(192000 - 30000);                          // B within the window: no abort, it closes again
    set(kParamStopClear, 1); runMod(true, 2); set(kParamStopClear, 0); runMod(true, 20);
    set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0);
    CHECK(eng().state(0) == ouro::TrackState::Recording && eng().userWaiting(0));
    runTo(192000 + 128 * 200); CHECK(eng().loopLength() == 192000 && eng().state(0) == ouro::TrackState::Playing);
}
// C leaves another track's wait alone and acts on the selected track: B waits on track 1, the knob turns to track 2
// (its loop), C just before the line. Track 1 starts on the line; track 2 goes off and stays.
static void cLeavesAWaitOnAnotherTrack() {
    fresh(); gT = 0; set(kParamSync, 1); set(kParamTrack, 2); runMod(true, 1);
    set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0);           // track 2's first loop: one bar
    runTo(96000 - 1280); set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0); runTo(96000 + 128 * 20);
    CHECK(eng().loopLength() == 96000 && eng().state(1) == ouro::TrackState::Playing);
    set(kParamTrack, 1); runMod(true, 1);
    runTo(2 * 96000 + 30000); set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0);   // track 1 waits for the line
    CHECK(eng().userWaiting(0) && eng().waitingTrack() == 0);
    set(kParamTrack, 2); runMod(true, 1); runTo(3 * 96000 - 1280);
    set(kParamStopClear, 1); runMod(true, 1); CHECK(eng().userWaiting(0) && !eng().isOn(1));
    runMod(true, 19); set(kParamStopClear, 0); runMod(true, 200);
    CHECK(eng().state(0) == ouro::TrackState::Recording && !eng().waiting());
    CHECK(eng().state(1) == ouro::TrackState::Playing && !eng().isOn(1));
}

// k blocks of MOD's transport; the Click output of each block appended to `click`.
static void runModClick(bool playing, int k, std::vector<float>& click) {
    for (int i = 0; i < k; ++i) { runMod(playing, 1); click.insert(click.end(), gClick.begin(), gClick.end()); }
}
// Where each tick starts: the first sample above the floor after at least 1000 below it.
static std::vector<size_t> tickStarts(const std::vector<float>& x) {
    std::vector<size_t> s; size_t quiet = 1000;
    for (size_t i = 0; i < x.size(); ++i) {
        if (std::fabs(x[i]) > 1e-4f) { if (quiet >= 1000) s.push_back(i); quiet = 0; } else ++quiet;
    }
    return s;
}
static float peakFrom(const std::vector<float>& x, size_t at, size_t n) {
    float p = 0.0f;
    for (size_t i = at; i < std::min(x.size(), at + n); ++i) p = std::max(p, std::fabs(x[i]));
    return p;
}
static void clickTicksOnModsBeats() {
    fresh(); gT = 0; set(kParamSync, 1); set(kParamClick, 1); set(kParamClickLevel, 0);
    std::vector<float> c; runModClick(true, 1500, c);                // 192000 samples: two bars
    const std::vector<size_t> s = tickStarts(c);
    CHECK(s.size() == 8);
    for (size_t k = 0; k < s.size(); ++k) CHECK(s[k] >= 24000 * k && s[k] <= 24000 * k + 2);   // every beat, from bar 1's line
    CHECK(peakFrom(c, 0, 1200) > 0.9f && peakFrom(c, 96000, 1200) > 0.9f);   // beat 1: full level
    const float weak = peakFrom(c, 24000, 1200);
    CHECK(weak > 0.4f && weak < 0.6f);                               // the other beats: -6 dB
    CHECK(peakFrom(c, 1200, 24000 - 1200) == 0.0f);                  // 25 ms, then silence
    fresh(); gT = 0; set(kParamSync, 1); set(kParamClick, 1); set(kParamClickLevel, -20);
    c.clear(); runModClick(true, 10, c);
    CHECK(peakFrom(c, 0, 1200) > 0.09f && peakFrom(c, 0, 1200) < 0.11f);   // Click Level
    fresh(); gT = 50000; set(kParamSync, 1); set(kParamClick, 1); set(kParamClickLevel, 0);   // MOD starts in beat 3
    c.clear(); runModClick(true, 600, c);                            // 76800 samples: beats 4, 1, 2
    const std::vector<size_t> m = tickStarts(c);
    CHECK(m.size() == 3);
    if (m.size() == 3) {
        CHECK(m[0] >= 22000 && m[0] <= 22002 && m[1] >= 46000 && m[1] <= 46002 && m[2] >= 70000 && m[2] <= 70002);
        CHECK(peakFrom(c, m[0], 1200) < 0.6f && peakFrom(c, m[1], 1200) > 0.9f && peakFrom(c, m[2], 1200) < 0.6f);
    }
}
static void clickIsSilentUnlessOnSyncedAndPlaying() {
    std::vector<float> c;
    fresh(); gT = 0; set(kParamSync, 1); runModClick(true, 400, c);  // Click off
    CHECK(peakFrom(c, 0, c.size()) == 0.0f);
    fresh(); gT = 0; set(kParamSync, 1); set(kParamClick, 1); c.clear(); runModClick(false, 400, c);   // MOD stopped
    CHECK(peakFrom(c, 0, c.size()) == 0.0f);
    fresh(); gT = 0; set(kParamClick, 1); c.clear(); runModClick(true, 400, c);   // Sync off
    CHECK(peakFrom(c, 0, c.size()) == 0.0f);
    fresh(); gT = 0; set(kParamSync, 1); set(kParamClick, 1); c.clear(); runModClick(true, 10, c);
    set(kParamClick, 0); c.clear(); runModClick(true, 400, c);      // turned off: silent from then on
    CHECK(peakFrom(c, 0, c.size()) == 0.0f);
}
static void lengthReachesTheEngine() {
    fresh(); gT = 0; set(kParamSync, 1); set(kParamLength, 1); runMod(true, 1);
    set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0);          // on bar 1's line (back-dated)
    runMod(true, 760);                                              // past bar 2's line, no second press
    CHECK(eng().state(0) == ouro::TrackState::Playing && eng().loopLength() == 96000);
}

// A 188-block loop on track 1 (as recordBaseAndDub's base), its tail done.
static void recordLoop()
{
    set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(187);
    set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(40);
}
// Runs block by block until f() holds, at most max blocks; false if it never did.
template <class F> static bool runUntil(F f, int max = 2000)
{
    for (int k = 0; k < max && !f(); ++k) runN(1);
    return f();
}
static void song(std::initializer_list<std::pair<int, int>> steps)   // the hidden ports: part x repeats from step 1
{
    uint32_t i = kParamStep1Part;
    for (const auto& s : steps) { set(i++, float(s.first)); set(i++, float(s.second)); }
}

static void partPortSwitchesAtThePartEnd() {
    fresh(); recordLoop(); gReqs.clear();
    set(kParamPart, 1); runN(1); CHECK(eng().queuedPart() == 1 && eng().part() == 0);
    CHECK(!hasReq(kParamPart, 0));                                     // the knob shows the part picked
    CHECK(runUntil([] { return eng().part() == 1; }, 200) && eng().queuedPart() == -1);
}
static void knobFollowsTheSong() {
    fresh(); song({{0, 1}, {1, 1}, {4, 1}}); recordLoop(); gReqs.clear();   // A x1, B x1, Loop
    set(kParamSong, 1); runN(1); CHECK(eng().songOn());
    CHECK(runUntil([] { return eng().part() == 1; }) && hasReq(kParamPart, 1));
    gReqs.clear(); CHECK(runUntil([] { return eng().part() == 0; }) && hasReq(kParamPart, 0));   // Loop: A again
    CHECK(eng().songOn() && P().getParameterValue(kParamSong) == 1.0f);    // its own requests are no hand on Part
}
static void handOnPartTurnsSongPortOff() {
    fresh(); song({{0, 1}, {1, 1}, {4, 1}}); recordLoop();
    set(kParamSong, 1); CHECK(runUntil([] { return eng().part() == 1; }));
    runN(1); gReqs.clear(); set(kParamPart, 0); runN(1);
    CHECK(hasReq(kParamSong, 0) && !eng().songOn() && eng().queuedPart() == 0);
}
static void stepProxyEditsTheSelectedStep() {
    fresh(); song({{0, 1}, {1, 2}, {2, 3}}); runN(1); gReqs.clear();
    set(kParamStep, 3); runN(1); CHECK(hasReq(kParamStepPart, 2) && hasReq(kParamStepRepeats, 3));
    set(kParamStepRepeats, 4); runN(1);
    CHECK(hasReq(kParamStep3Repeats, 4) && eng().stepRepeats(2) == 4 && eng().stepPart(2) == 2);
    set(kParamStepPart, 5); runN(1); CHECK(hasReq(kParamStep3Part, 5) && eng().stepPart(2) == ouro::kStepStop);
    gReqs.clear(); set(kParamStep2Repeats, 7); runN(1);                 // another step's port: the proxies stay
    CHECK(eng().stepRepeats(1) == 7 && gReqs.empty());
    set(kParamStep3Repeats, 9); runN(1);                                // the selected step's port: they follow
    CHECK(eng().stepRepeats(2) == 9 && hasReq(kParamStepRepeats, 9));
    gReqs.clear(); set(kParamStep, 1); runN(1); CHECK(hasReq(kParamStepPart, 0) && hasReq(kParamStepRepeats, 1));
}
static void hiddenStepPortsRestoreTheArrangement() {
    fresh(); song({{0, 1}, {1, 2}, {5, 1}}); runN(1);                   // as a pedalboard load sets them
    CHECK(eng().stepPart(1) == 1 && eng().stepRepeats(1) == 2 && eng().stepPart(2) == ouro::kStepStop);
    CHECK(eng().songLength() == 2);
    recordLoop(); set(kParamSong, 1);                                  // A x1, B x2, Stop
    CHECK(runUntil([] { return eng().songStep() == 1; }) && eng().part() == 1);
    CHECK(runUntil([] { return eng().transport() == ouro::Transport::Stopped; }) && eng().songStep() == -1);
}
static void stopStepUsesTheStopModePort() {
    fresh(); song({{0, 1}, {5, 1}}); set(kParamStopMode, 2); set(kParamFadeTime, 0.5f); recordLoop();
    set(kParamSong, 1);
    CHECK(runUntil([] { return eng().transport() == ouro::Transport::Stopping; }));
    runN(90); CHECK(eng().transport() == ouro::Transport::Stopping);  // 240 ms into a 500 ms fade
    runN(110); CHECK(eng().transport() == ouro::Transport::Stopped);
}
static void onAndVolumePortsFollowThePart() {
    fresh(); recordLoop(); set(kParamOn2, 0); runN(1);
    set(kParamPart, 1); CHECK(runUntil([] { return eng().part() == 1; }));
    CHECK(!eng().isOn(1));                                              // B filled from A
    set(kParamOn2, 1); set(kParamVolume1, -12); runN(1); CHECK(eng().isOn(1) && eng().volumeDb(0) == -12.0f);
    gReqs.clear(); set(kParamPart, 0); CHECK(runUntil([] { return eng().part() == 0; }));
    CHECK(hasReq(kParamOn2, 0) && hasReq(kParamVolume1, 0) && hasReq(kParamVolume, 0));
    gReqs.clear(); set(kParamPart, 1); CHECK(runUntil([] { return eng().part() == 1; }));
    CHECK(hasReq(kParamOn2, 1) && hasReq(kParamVolume1, -12) && hasReq(kParamVolume, -12));
}
static void loadWithSongOnStartsInStepOnesPart() {
    fresh(); set(kParamStep1Part, 2); set(kParamSong, 1); runN(1);
    CHECK(eng().part() == 2 && hasReq(kParamPart, 2) && eng().songOn());
    fresh(); set(kParamPart, 1); set(kParamStep1Part, 2); set(kParamSong, 1); runN(1);   // a saved Part as well
    CHECK(eng().part() == 2 && hasReq(kParamPart, 2) && eng().songOn() && !hasReq(kParamSong, 0));
    fresh(); set(kParamPart, 3); runN(1); CHECK(eng().part() == 3 && !eng().songOn());   // Song off: the saved Part
}
static void clearAllRequestsPartA() {
    fresh(); recordLoop(); set(kParamPart, 2); CHECK(runUntil([] { return eng().part() == 2; }));
    gReqs.clear(); set(kParamClearAll, 1); runN(1); set(kParamClearAll, 0); runN(10);
    CHECK(eng().part() == 0 && eng().loopLength() == 0 && hasReq(kParamPart, 0));
}

// Saved pieces. MOD_USER_FILES_DIR is a fresh folder (main()); each test starts with no slot. The worker thread is real:
// waits are on conditions, with a wall-clock limit.
static std::string gRoot;                                           // its "Audio Loops/Ouroboros"
static void freshPieces() { ouro::pieces::removeTree(gRoot); fresh(); }
static std::string status() { return plug().pieceStatus(); }
static void press(uint32_t i) { set(i, 1); runN(1); set(i, 0); runN(1); }
template <class F> static bool waitFor(F f)                         // blocks until f() holds; false after 5 s
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!f()) { if (std::chrono::steady_clock::now() > end) return false; runN(1); }
    return true;
}
static bool waitPieces() { return waitFor([] { return !plug().pieceLoading() && status() != "SAVING"; }); }
static void saveTo(int slot) { set(kParamSlot, float(slot)); press(kParamSave); CHECK(waitPieces()); }
static bool stopped() { return eng().transport() == ouro::Transport::Stopped; }
// Stopped: Stop/Play plays the loop from the top; that pass, then stopped again.
static std::vector<float> passFromTheTop()
{
    set(kParamStopPlay, 1); run(P(), sine(220, 0.1f, eng().loopLength()));
    const std::vector<float> o = gOut;
    set(kParamStopPlay, 0); runN(1); press(kParamStopPlay); CHECK(runUntil(stopped));
    return o;
}
static std::string slurp(const std::string& path)
{
    std::string s;
    if (FILE* f = std::fopen(path.c_str(), "rb")) { char b[4096]; size_t n; while ((n = std::fread(b, 1, sizeof b, f))) s.append(b, n); std::fclose(f); }
    return s;
}
static std::string stamp(const std::string& path)                   // a save replaces the slot: a new file, a new inode
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return "";
    return std::to_string(st.st_ino) + ":" + std::to_string(st.st_mtim.tv_sec) + "." + std::to_string(st.st_mtim.tv_nsec);
}

static void saveAndLoadRoundTrip() {
    freshPieces(); recordBaseAndDub(); gReqs.clear();
    saveTo(2); CHECK(status() == "SAVED 2" && hasReq(kParamPiece, 2));
    press(kParamStopPlay); CHECK(runUntil(stopped));
    const std::vector<float> want = passFromTheTop();
    press(kParamClearAll); CHECK(eng().empty());
    press(kParamLoad); CHECK(plug().pieceLoading() && status() != "LOAD 2?");   // nothing to lose: no question
    CHECK(waitPieces() && stopped() && eng().undoDepth(0) == 0);
    const std::vector<float> got = passFromTheTop();
    CHECK(got.size() == want.size() && maxAbsDiff(got, want) < 1e-6f && peak(got) > 0.05f);
}
static void saveWaitsForTheTake() {
    freshPieces(); recordLoop();
    press(kParamRec); runN(10);                                      // an overdub runs
    set(kParamSlot, 2); press(kParamSave); runN(100);
    CHECK(status() == "SAVING" && stamp(gRoot + "/02/piece.txt").empty());
    press(kParamRec); CHECK(waitPieces() && status() == "SAVED 2");  // closed: now the snapshot
}
static void saveOnAFullSlotAsksFirst() {
    freshPieces(); recordLoop(); saveTo(2);
    const std::string txt = gRoot + "/02/piece.txt", t0 = stamp(txt);
    CHECK(!t0.empty());
    press(kParamSave); CHECK(status() == "SAVE 2?");
    set(kParamSlot, 3); runN(1); CHECK(status() == "3 EMPTY");      // a Slot change cancels
    set(kParamSlot, 2); press(kParamSave); CHECK(status() == "SAVE 2?");   // so this asks again
    CHECK(waitPieces() && stamp(txt) == t0);                         // nothing written
    press(kParamSave); CHECK(waitPieces() && status() == "SAVED 2" && !stamp(txt).empty() && stamp(txt) != t0);
}
static void loadWithChangesAsksFirst() {
    freshPieces(); recordLoop(); saveTo(2);
    press(kParamRec); runN(40); press(kParamRec); runN(40);         // an overdub since the save
    press(kParamLoad); CHECK(status() == "LOAD 2?" && !plug().pieceLoading());
    runN(750); CHECK(status() == "2 FULL");                          // 2 s later the question has lapsed
    press(kParamLoad); CHECK(status() == "LOAD 2?" && !plug().pieceLoading());
    press(kParamLoad); CHECK(plug().pieceLoading() && waitPieces() && eng().undoDepth(0) == 0 && stopped());
    press(kParamLoad); CHECK(plug().pieceLoading() && waitPieces());   // as loaded: nothing to lose, no question
    press(kParamRec); runN(10); press(kParamRec); runN(10);         // R10: B plays, B dubs; the take still runs
    CHECK(eng().state(0) == ouro::TrackState::Overdubbing);
    press(kParamLoad); CHECK(status() == "LOAD 2?" && !plug().pieceLoading() && eng().state(0) == ouro::TrackState::Overdubbing);
    press(kParamLoad); CHECK(plug().pieceLoading() && waitPieces() && eng().undoDepth(0) == 0 && stopped());
    CHECK(eng().state(0) == ouro::TrackState::Playing);
}
static void pressesDuringALoadAreIgnored() {
    freshPieces(); recordLoop(); saveTo(2);
    set(kParamLoad, 1); runN(1); set(kParamLoad, 0); CHECK(plug().pieceLoading() && status() == "LOADING 0%");
    set(kParamRec, 1); runN(1);                                      // pressed during the load, held past its end
    CHECK(waitPieces()); runN(1); set(kParamRec, 0); runN(1);
    for (int t = 0; t < 4; ++t) CHECK(eng().state(t) != ouro::TrackState::Recording);
    CHECK(stopped());
    press(kParamStopPlay); tapC();                                   // a running tap: off, the window open
    CHECK(eng().transport() == ouro::Transport::Running && !eng().isOn(0));
    press(kParamLoad); CHECK(status() == "LOAD 2?");                 // the tap is a change: asked, then confirmed
    press(kParamLoad); CHECK(plug().pieceLoading() && waitPieces() && eng().isOn(0));
    cDown(); CHECK(eng().isOn(0)); cUp();                            // the load ended the window: no double-tap
    CHECK(eng().state(0) == ouro::TrackState::Playing);
}
static void dryLevelHoldsThroughALoad() {                         // the lent blocks pass the input at Dry's level
    freshPieces(); set(kParamDry, -6.0f); recordLoop(); saveTo(2);
    press(kParamStopPlay); CHECK(runUntil(stopped));
    std::vector<float> want = sine(220, 0.1f, 128);                  // what runN(1) feeds
    for (float& v : want) v *= ouro::dryGain(-6.0f);
    set(kParamLoad, 1); runN(1); set(kParamLoad, 0); CHECK(plug().pieceLoading());
    bool level = true;
    CHECK(waitFor([&] { level = level && maxAbsDiff(gOut, want) < 1e-6f; return !plug().pieceLoading(); }));
    CHECK(level); CHECK(stopped()); CHECK(plug().loadedPiece() == 2);
}
static void settingsDuringALoadApplyAfter() {                      // R9: a setting waits for the engine, not dropped
    freshPieces(); recordLoop(); saveTo(2);
    set(kParamLoad, 1); runN(1); set(kParamLoad, 0); CHECK(plug().pieceLoading());
    set(kParamDry, 0.0f); set(kParamPart, 2); runN(1);               // during the load
    CHECK(waitPieces() && stopped());
    const std::vector<float> s = sine(220, 0.1f, 4800); run(P(), s);
    CHECK(maxAbsDiff(slice(gOut, 2400, 4800), slice(s, 2400, 4800)) < 1e-6f);   // stopped: the dry input alone
    CHECK(eng().part() == 0 && P().getParameterValue(kParamPart) == 0.0f);       // Part is the piece's: dropped
}
static void controlsFollowTheLoadedPiece() {
    freshPieces(); recordLoop();
    set(kParamPart, 1); CHECK(runUntil([] { return eng().part() == 1; }));   // B, filled from A on entry
    set(kParamVolume1, -6); press(kParamStopPlay); CHECK(runUntil(stopped));
    set(kParamStep1Part, 1); set(kParamStep2Part, 2); set(kParamStep2Repeats, 3); set(kParamSong, 1); runN(1);
    CHECK(eng().part() == 1 && eng().songOn() && eng().volumeDb(0) == -6.0f);   // stopped: step 1's part (B)
    saveTo(2);
    fresh(); set(kParamSlot, 2); set(kParamStep, 2); CHECK(waitFor([] { return status() == "2 FULL"; }));   // the first scan
    gReqs.clear(); press(kParamLoad); CHECK(waitPieces());
    CHECK(hasReq(kParamPart, 1) && hasReq(kParamSong, 1) && hasReq(kParamStep2Part, 2) && hasReq(kParamStep2Repeats, 3));
    CHECK(hasReq(kParamStepPart, 2) && hasReq(kParamStepRepeats, 3));   // the proxies show step 2
    CHECK(hasReq(kParamVolume1, -6) && hasReq(kParamPiece, 2) && eng().part() == 1 && eng().songOn());
    press(kParamLoad); CHECK(plug().pieceLoading() && waitPieces());   // the load's own changes are no reason to ask
}
static void autoLoadOnFirstRun() {
    freshPieces(); recordLoop(); set(kParamVolume1, -6); saveTo(2);
    const uint32_t len = eng().loopLength();
    fresh(); set(kParamPiece, 2); runN(1);                           // a pedalboard that remembers piece 2
    CHECK(plug().pieceLoading() && waitPieces());
    CHECK(hasReq(kParamSlot, 2) && eng().loopLength() == len && eng().state(0) == ouro::TrackState::Playing && stopped());
    CHECK(hasReq(kParamVolume1, -6) && hasReq(kParamVolume, -6));    // part A both before and after: pushed all the same
}
// The result lands while the Dwarf may show page 1: it stays until the next Slot change, Save or Load (5 s: 1875 blocks).
static void autoLoadOfABrokenSlotShowsWhy() {
    freshPieces(); set(kParamPiece, 7); runN(1);                     // slot 7 holds nothing
    set(kParamSlot, 5);                                              // turned during the load: the status stays
    CHECK(waitPieces() && status() == "EMPTY" && eng().empty() && hasReq(kParamPiece, 0));
    runN(1875); CHECK(status() == "EMPTY");
    set(kParamSlot, 6); runN(1); CHECK(status() == "6 EMPTY");
    ouro::pieces::makeDirs(gRoot + "/07");
    if (FILE* f = std::fopen((gRoot + "/07/piece.txt").c_str(), "wb")) { std::fputs("ouroboros-piece 1\nrate 48000\n", f); std::fclose(f); }
    fresh(); set(kParamPiece, 7); runN(1);                           // a corrupt piece.txt
    CHECK(waitPieces() && status() == "LOAD ERROR" && eng().empty() && hasReq(kParamPiece, 0));
    runN(1875); CHECK(status() == "LOAD ERROR");
    set(kParamSlot, 2); runN(1); CHECK(status() == "2 EMPTY");
}
// A save fails while the user plays on page 1: the failure stays as a load's does; SAVED goes after 3 s.
static void aFailedSaveStaysOnScreen() {
    freshPieces(); recordLoop();
    ouro::pieces::makeDirs(gRoot); chmod(gRoot.c_str(), 0555);       // no slot folder can be made
    saveTo(2); CHECK(status() == "SAVE ERROR");
    runN(1875); CHECK(status() == "SAVE ERROR");
    set(kParamSlot, 3); runN(1); CHECK(status() == "3 EMPTY");
    chmod(gRoot.c_str(), 0755);
    saveTo(3); CHECK(status() == "SAVED 3");
    runN(1200); CHECK(status() == "3 FULL");
}
// Load on an empty slot does nothing: the loops play on, sample for sample as without the press.
static void loadOfAnEmptySlotKeepsPlaying() {
    std::vector<float> out[2];
    for (int k = 0; k < 2; ++k) {
        freshPieces(); recordLoop(); set(kParamSlot, 3); runN(1);
        if (k) press(kParamLoad);
        else { runN(1); runN(1); }                                   // press()'s blocks, no press
        if (k) CHECK(status() == "EMPTY" && !plug().pieceLoading() && eng().transport() == ouro::Transport::Running);
        runN(400); out[k] = gOut;
    }
    CHECK(maxAbsDiff(out[0], out[1]) == 0.0f && peak(out[1]) > 0.05f);
}
static void saveStoresTheBpm() {
    freshPieces(); recordLoop();
    gT = 0; set(kParamSync, 1); runMod(true, 1);                     // MOD at 120 BPM
    saveTo(2); CHECK(slurp(gRoot + "/02/piece.txt").find("\nbpm 120.000\n") != std::string::npos);
    set(kParamSync, 0); saveTo(3); CHECK(slurp(gRoot + "/03/piece.txt").find("\nbpm 0.000\n") != std::string::npos);
    set(kParamSlot, 2); press(kParamLoad); CHECK(waitPieces() && status() == "120 BPM 1 BAR");   // a free loop: 1 bar
    runN(1200); CHECK(status() == "120 BPM 1 BAR");                  // until the next Slot change, Save or Load
    set(kParamSlot, 3); runN(1); CHECK(status() == "3 FULL");
    press(kParamLoad); CHECK(waitPieces() && status() == "1 BAR");
}

static void dubB(int blocks) { set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(blocks); set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(40); }

static void holdBUndoesOneLayerPerHold() {
    fresh(); recordBaseAndDub(); dubB(40);                              // depth 2
    bDown(); runN(188); CHECK(eng().undoDepth(0) == 1 && eng().state(0) == ouro::TrackState::Playing);
    bUp(); runN(10);
    bDown(); runN(188); CHECK(eng().undoDepth(0) == 0); bUp(); runN(10);
    bDown(); runN(188); CHECK(eng().undoDepth(0) == 0 && eng().state(0) == ouro::TrackState::Playing);  // first kept
    bUp();
}
static void holdBDuringAnOverdubUndoesIt() {
    fresh(); recordBaseAndDub();                                        // depth 1
    set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(60);           // an overdub running
    bDown(); CHECK(eng().undoDepth(0) == 2);                            // the press closed it
    runN(188); CHECK(eng().undoDepth(0) == 1); bUp();
}
static void holdBAfterRecDubKeepsTheRecording() {                       // Review Focus 1
    fresh(); set(kParamAfterRec, 1);
    set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(187);
    bDown(); CHECK(eng().state(0) == ouro::TrackState::Overdubbing);    // closed into an overdub
    runN(188); CHECK(eng().state(0) == ouro::TrackState::Playing && eng().loopLength() > 0 && eng().undoDepth(0) == 0);
    bUp();
}
static void holdBOnAnArmedEmptyTrackKeepsTheArm() {
    fresh(); set(kParamAutoRec, 1); set(kParamThreshold, -10); bDown(); runN(188); CHECK(eng().armedTrack() == 0); bUp();
}
static void redoAfterHoldsBringsBackOnlyTheLast() {
    fresh(); recordBaseAndDub(); dubB(40);                              // depth 2
    bDown(); runN(188); bUp(); runN(10); bDown(); runN(188); bUp(); runN(10);
    CHECK(eng().undoDepth(0) == 0);
    set(kParamRedo, 1); runN(1); set(kParamRedo, 0); runN(10); CHECK(eng().undoDepth(0) == 1);
    set(kParamRedo, 1); runN(1); set(kParamRedo, 0); runN(10); CHECK(eng().undoDepth(0) == 1 && !eng().canRedo(0));
}
static void holdBFromStoppedStartsThenUndoes() {
    fresh(); recordBaseAndDub();
    set(kParamStopPlay, 1); runN(1); set(kParamStopPlay, 0); runN(200);
    CHECK(eng().transport() == ouro::Transport::Stopped && eng().undoDepth(0) == 1);
    bDown(); CHECK(eng().transport() == ouro::Transport::Running);
    runN(188); CHECK(eng().undoDepth(0) == 0 && eng().transport() == ouro::Transport::Running); bUp();
}
static void holdBDroppedOnPartChange() {
    fresh(); recordBaseAndDub();
    set(kParamPart, 1); runN(200); dubB(40);                            // part 1 gets a layer a wrong peel could take
    const int d1 = eng().undoDepth(0); CHECK(eng().part() == 1 && d1 >= 1);
    set(kParamPart, 0); runN(200); CHECK(eng().part() == 0);
    set(kParamHoldTime, 2000);
    set(kParamPart, 1); bDown(); runN(760);                             // switch at the loop end, before the hold time
    CHECK(eng().part() == 1 && eng().undoDepth(0) == d1); bUp();
}
static void cGestureKeepsTheTrackItWasPressedOn() {                    // Review Focus 2
    fresh(); recordLoop(); set(kParamTrack, 2); recordLoop();           // track 2 recorded onto the loop
    CHECK(eng().state(1) == ouro::TrackState::Playing);
    set(kParamTrack, 1); runN(1); cDown(); set(kParamTrack, 2); runN(187);
    CHECK(eng().state(0) == ouro::TrackState::Empty && eng().state(1) != ouro::TrackState::Empty); cUp();
}
static void cGestureDroppedOnPartChange() {                            // Review Focus 4
    fresh(); set(kParamHoldTime, 2000); recordLoop();
    cDown(); set(kParamPart, 1); runN(760);                             // the switch at the loop end, then past the hold
    CHECK(eng().part() == 1 && eng().state(0) != ouro::TrackState::Empty); cUp();
}

// C while stopped. Two tracks recorded (track 1, then track 2 onto the loop), track 1 selected, stopped.
static void twoTracksStoppedNoFresh() {
    recordLoop(); set(kParamTrack, 2); runN(1); recordLoop(); set(kParamTrack, 1); runN(1);
    press(kParamStopPlay); CHECK(runUntil(stopped)); gReqs.clear();
}
static void twoTracksStopped() { fresh(); twoTracksStoppedNoFresh(); }
static void cTapWhileStoppedStartsTheSelectedTrackAlone() {
    twoTracksStopped(); set(kParamTrack, 2); runN(1);
    cDown(); CHECK(stopped()); runN(19); cUp();
    CHECK(eng().transport() == ouro::Transport::Running && eng().isOn(1) && !eng().isOn(0) && hasReq(kParamOn1, 0.0f));
}
static void cSoloStartLeavesEmptyTracksOn() {
    twoTracksStopped(); tapC(); CHECK(eng().isOn(2) && eng().isOn(3));
}
static void cDoubleTapWhileStoppedStartsEverything() {
    twoTracksStopped(); set(kParamTrack, 2); runN(1); tapC(); runN(100); cDown(); cUp(); runN(60);
    CHECK(eng().transport() == ouro::Transport::Running && eng().isOn(0) && eng().isOn(1));
    CHECK(eng().position() < 128 * 80);                                 // restarted from the top at the second press
}
static void cHoldWhileStoppedClearsTheTrackThenEverything() {            // Ruling R3: Clear All leaves the fresh engine
    twoTracksStopped(); set(kParamTrack, 2); runN(1);
    cDown(); runN(188); CHECK(eng().state(1) == ouro::TrackState::Empty && eng().state(0) != ouro::TrackState::Empty && stopped());
    runN(185); CHECK(eng().state(0) != ouro::TrackState::Empty && stopped());   // nothing started before twice Hold Time
    runN(3); CHECK(eng().empty() && eng().loopLength() == 0 && eng().transport() == ouro::Transport::Running);
    cUp(); runN(10); CHECK(eng().empty() && eng().loopLength() == 0 && eng().transport() == ouro::Transport::Running);
    twoTracksStopped(); set(kParamTrack, 2); runN(1);                   // released after the track's clear: no tap,
    cDown(); runN(188); cUp(); runN(5); tapC(); runN(10);               // so the next press is no double-tap
    CHECK(stopped() && eng().state(0) != ouro::TrackState::Empty && eng().isOn(0));
}
static void cTapWhileStoppedOnAnEmptyTrackDoesNothing() {
    twoTracksStopped(); set(kParamTrack, 3); runN(1); tapC(); runN(160); CHECK(stopped());
}
static void cSoloStartCountsAsAChange() {                               // Review Focus 5
    freshPieces(); twoTracksStoppedNoFresh(); saveTo(2);
    set(kParamTrack, 2); runN(1); tapC(); runN(160);
    press(kParamStopPlay); CHECK(runUntil(stopped));
    press(kParamLoad); CHECK(status() == "LOAD 2?");
}
// Sync on, MOD playing mid-bar: the tap's restart waits for the line (96000) with track 1 off.
static void syncTapWaits() {
    twoTracksStopped(); gT = 50000; set(kParamSync, 1); set(kParamTrack, 2); runMod(true, 1);
    set(kParamStopClear, 1); runMod(true, 20); set(kParamStopClear, 0); runMod(true, 1);
    CHECK(eng().userWaiting() && !eng().isOn(0) && eng().isOn(1));
}
static void cSyncWaitPutsTracksBackWhenCancelled() {
    syncTapWaits(); set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0); runMod(true, 1);    // B cancels
    CHECK(!eng().userWaiting() && stopped() && eng().isOn(0) && eng().isOn(1));
    syncTapWaits(); runMod(true, 160);                                  // the window passes; then a C press cancels
    set(kParamStopClear, 1); runMod(true, 1); set(kParamStopClear, 0); runMod(true, 1);
    CHECK(!eng().userWaiting() && stopped() && eng().isOn(0) && eng().isOn(1));
    syncTapWaits(); runMod(true, 10);                                   // a second press within the window
    set(kParamStopClear, 1); runMod(true, 1); set(kParamStopClear, 0); runMod(true, 1);
    CHECK(eng().userWaiting() && stopped() && eng().isOn(0) && eng().isOn(1));
    runTo(96000 + 128 * 5); CHECK(eng().transport() == ouro::Transport::Running && eng().isOn(0) && eng().isOn(1));
}
// A restart queued while C is held (MOD starts mid-bar) is kept by the tap: it becomes the solo start.
static void cTapKeepsARestartQueuedWhileHeld() {
    twoTracksStopped(); gT = 50000; set(kParamSync, 1); set(kParamTrack, 2); runMod(false, 1);
    set(kParamStopClear, 1); runMod(false, 1); runMod(true, 1); CHECK(eng().userWaiting() && stopped());
    runMod(true, 15); set(kParamStopClear, 0); runMod(true, 1);
    CHECK(eng().userWaiting() && stopped() && !eng().isOn(0) && eng().isOn(1));
    runTo(96000 + 128 * 5); CHECK(eng().transport() == ouro::Transport::Running && !eng().isOn(0) && eng().isOn(1));
}
// T4: a load during a solo WAIT keeps the loaded piece's On (the tap's put-back is dropped with the load).
static void loadDuringASoloWaitKeepsThePiecesOn() {
    freshPieces(); twoTracksStoppedNoFresh(); set(kParamOn1, 0); runN(1); saveTo(2);   // the piece: track 1 off
    set(kParamOn1, 1); runN(1);
    gT = 50000; set(kParamSync, 1); set(kParamTrack, 2); runMod(true, 1);
    set(kParamStopClear, 1); runMod(true, 20); set(kParamStopClear, 0); runMod(true, 1);
    CHECK(eng().userWaiting() && !eng().isOn(0) && eng().isOn(1));
    press(kParamLoad); CHECK(status() == "LOAD 2?");
    press(kParamLoad); CHECK(plug().pieceLoading() && waitPieces()); runN(1);
    CHECK(stopped() && !eng().isOn(0) && eng().isOn(1) && P().getParameterValue(kParamOn1) == 0.0f);
}
// R10: Song on (A x1, B x1, Stop), tracks 1 and 2 in A and B (B filled from A), stopped by the song's end in B: the
// engine sits in step 1's part (the Part port with it), so C acts there.
static void songEndedInB() {
    fresh(); song({{0, 1}, {1, 1}, {5, 1}});
    recordLoop(); set(kParamTrack, 2); runN(1); recordLoop();
    set(kParamSong, 1); CHECK(runUntil([] { return eng().part() == 1; }) && eng().isOn(0) && eng().isOn(1));
    CHECK(runUntil(stopped) && eng().songOn() && eng().songStep() == -1);
    CHECK(eng().part() == 0 && P().getParameterValue(kParamPart) == 0.0f);
}
static void cDoubleTapAcrossASongStopPutsBackAndStaysStopped() {        // the stop's park moves the song home inside the window
    fresh(); set(kParamStopMode, 2); set(kParamFadeTime, 0.5f); song({{0, 1}, {1, 1}, {5, 1}});
    recordLoop(); set(kParamTrack, 2); runN(1); recordLoop(); set(kParamTrack, 1); runN(1);
    set(kParamSong, 1); CHECK(runUntil([] { return eng().part() == 1; }));
    CHECK(runUntil([] { return eng().transport() == ouro::Transport::Stopping; }));   // the Stop step's fade, in B
    runN(120); tapC(); CHECK(!eng().isOn(0) && eng().transport() == ouro::Transport::Stopping);
    CHECK(runUntil(stopped, 120) && eng().part() == 0);                // parked in step 1's part, inside the window
    cDown(); cUp(); runN(10);
    CHECK(stopped() && eng().part() == 0 && eng().isOn(0) && eng().isOn(1));   // no solo start
    press(kParamStopPlay); CHECK(runUntil([] { return eng().part() == 1; }) && eng().isOn(0));   // B's track 1 put back
}
static void cAfterTheSongEndsActsInStepOnesPart() {
    songEndedInB(); tapC();                                             // track 2 alone, in A
    CHECK(eng().transport() == ouro::Transport::Running && eng().part() == 0 && !eng().isOn(0) && eng().isOn(1));
    CHECK(runUntil([] { return eng().part() == 1; }) && eng().isOn(0) && eng().isOn(1));   // B's On untouched
    songEndedInB(); doubleTap(); runN(10);                              // everything
    CHECK(eng().transport() == ouro::Transport::Running && eng().part() == 0 && eng().isOn(0) && eng().isOn(1));
}

int main()
{
    char dir[] = "build/tests/pluginXXXXXX";                         // pieces go to a fresh folder, never $HOME
    mkdir("build/tests", 0777);
    if (!mkdtemp(dir)) { std::printf("mkdtemp failed\n"); return 1; }
    setenv("MOD_USER_FILES_DIR", dir, 1);
    gRoot = ouro::pieces::piecesDir();
    d_nextBufferSize = 128;
    d_nextSampleRate = 48000.0;
    d_nextCanRequestParameterValueChanges = true;

    layout();
    silentAndMono();
    recActsOnRisingEdgeOnly();
    holdBUndoesOneLayerPerHold(); holdBDuringAnOverdubUndoesIt(); holdBAfterRecDubKeepsTheRecording();
    holdBOnAnArmedEmptyTrackKeepsTheArm(); redoAfterHoldsBringsBackOnlyTheLast(); holdBFromStoppedStartsThenUndoes();
    holdBDroppedOnPartChange();
    cPressMutesAndReleaseUnmutes(); cHoldClearsTheTrackAndSetsItOn(); cHoldOnAnOffTrackNeverSounds();
    cDoubleTapRestoresThenStops(); cDoubleTapPutsBackTheOnBeforeTheTap(); cDoubleTapRestoresBeforeAFadeStop(); cTapOnAnEmptyTrackDoesNothing();
    cDuringAnOverdubClosesItAndMutes(); cTapDuringATakeOnAnOffTrackTurnsItOn(); cCancelsOnlyTheSelectedTracksArm(); cDuringTheFirstRecording(); cTapPendingDoesNotWipeALoopBClosed();
    cGestureKeepsTheTrackItWasPressedOn(); cGestureDroppedOnPartChange();
    cTapWhileStoppedStartsTheSelectedTrackAlone(); cSoloStartLeavesEmptyTracksOn(); cDoubleTapWhileStoppedStartsEverything();
    cHoldWhileStoppedClearsTheTrackThenEverything(); cTapWhileStoppedOnAnEmptyTrackDoesNothing();
    cSyncWaitPutsTracksBackWhenCancelled(); cTapKeepsARestartQueuedWhileHeld();
    cAfterTheSongEndsActsInStepOnesPart();
    cDoubleTapAcrossASongStopPutsBackAndStaysStopped();
    doubleTapStops();
    bRestartsAfterStop();
    doubleTapThenHoldDoesNotClear();
    doubleTapBegunWhileStoppingOnlyStops();
    doubleTapDuringAShortFirstRecording();
    stopPlayPortToggles();
    stopAndAutoRecSettingsReachTheEngine();
    undoRedoClearTriggers();
    trackSelectRetargets();
    volumeProxyFollowsSelectedTrack();
    onoffFlipsSelectedAndReportsPort();
    settingsReachTheEngine();
    bypassSemantics();
    savedValuesSnapOnLoad();
    reactivateResets();
    disabledEngineStillPassesDry();
    widgetShowsLayerCounts();
    syncPressWaitsForTheBar();
    modPlayAndStopEdgesFollow();
    syncSwitchedOnWhilePlayingIsNoEdge();
    pressSeesThisBlocksGrid();
    cCancelsTheSelectedTracksWait();
    cLeavesAWaitOnAnotherTrack();
    clickTicksOnModsBeats();
    clickIsSilentUnlessOnSyncedAndPlaying();
    lengthReachesTheEngine();
    partPortSwitchesAtThePartEnd();
    knobFollowsTheSong();
    handOnPartTurnsSongPortOff();
    stepProxyEditsTheSelectedStep();
    hiddenStepPortsRestoreTheArrangement();
    stopStepUsesTheStopModePort();
    onAndVolumePortsFollowThePart();
    loadWithSongOnStartsInStepOnesPart();
    clearAllRequestsPartA();
    saveAndLoadRoundTrip();
    saveWaitsForTheTake();
    saveOnAFullSlotAsksFirst();
    loadWithChangesAsksFirst();
    pressesDuringALoadAreIgnored();
    cSoloStartCountsAsAChange();
    loadDuringASoloWaitKeepsThePiecesOn();
    settingsDuringALoadApplyAfter();
    dryLevelHoldsThroughALoad();
    controlsFollowTheLoadedPiece();
    autoLoadOnFirstRun();
    autoLoadOfABrokenSlotShowsWhy();
    aFailedSaveStaysOnScreen();
    loadOfAnEmptySlotKeepsPlaying();
    saveStoresTheBpm();
    gPlugin.reset();
    ouro::pieces::removeTree(dir);
    REPORT();
}
