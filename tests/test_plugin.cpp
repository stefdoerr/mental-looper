#include "OuroborosPlugin.hpp"
#include "src/DistrhoPluginInternal.hpp"
#include "signals.hpp"
#include "test_util.hpp"

#include <memory>

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
static const ouro::Looper& eng()
{
    return static_cast<OuroborosPlugin*>(P().getInstancePointer())->engine();
}

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

static void portSymbolsAreFrozen()
{
    fresh();
    static const char* kSym[] = { "rec","undo_redo","track","onoff","volume","undo","redo","clear_track",
        "clear_all","on_1","on_2","on_3","on_4","volume_1","volume_2","volume_3","volume_4","after_rec",
        "dry_thru","tail","hold_time","budget","offset","trigger_latency",
        "stop_play","stop_mode","fade_time","auto_rec","threshold","double_tap","sync","click","click_level","length" };
    for (uint32_t i = 0; i < 34; ++i) CHECK(std::strcmp(P().getParameterSymbol(i), kSym[i]) == 0);
    CHECK(P().getParameterDesignation(kParamBypass) == kParameterDesignationBypass);
    CHECK(kParamBypass == 34 && P().getParameterCount() == 35);
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
    CHECK(P().isParameterTrigger(kParamRec) && !P().isParameterTrigger(kParamUndoRedo));
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
}

static void silentAndMono()
{
    fresh();
    CHECK(DISTRHO_PLUGIN_NUM_INPUTS == 1 && DISTRHO_PLUGIN_NUM_OUTPUTS == 2);   // the loops, then the click
    CHECK(std::strcmp(P().getAudioPort(false, 1).symbol, "click_out") == 0 && P().getAudioPort(false, 1).groupId == kPortGroupNone);
    CHECK(std::strcmp(P().getAudioPort(false, 0).symbol, "audio_out_1") == 0 && P().getAudioPort(false, 0).groupId == kPortGroupMono);
    P().activate();
    run(P(), sine(220, 0.1f, 4800));        // nothing recorded, dry_thru off -> silence
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

static void tapUndoHoldRedo()
{
    fresh();
    recordBaseAndDub();
    CHECK(eng().undoDepth(0) == 1);
    set(kParamUndoRedo, 1); runN(37); set(kParamUndoRedo, 0); runN(152);    // ~99 ms tap, then the double-tap window
    CHECK(eng().undoDepth(0) == 0 && eng().canRedo(0));
    set(kParamUndoRedo, 1); runN(190);                                     // ~507 ms held
    CHECK(eng().undoDepth(0) == 1);                                        // redo fired while held
    set(kParamUndoRedo, 0); runN(1); CHECK(eng().undoDepth(0) == 1);       // no undo on release
    // a hold_time of 2000 ms turns the same 507 ms hold into a plain undo on release
    set(kParamHoldTime, 2000); set(kParamUndoRedo, 1); runN(190); CHECK(eng().undoDepth(0) == 1);
    set(kParamUndoRedo, 0); runN(152); CHECK(eng().undoDepth(0) == 0);
}

static void tapC() { set(kParamUndoRedo, 1); runN(20); set(kParamUndoRedo, 0); runN(1); }
static void doubleTap() { tapC(); runN(29); set(kParamUndoRedo, 1); runN(1); set(kParamUndoRedo, 0); runN(1); }

static void tapUndoWaitsForTheWindow() {
    fresh(); recordBaseAndDub(); tapC();
    CHECK(eng().undoDepth(0) == 1); runN(148); CHECK(eng().undoDepth(0) == 1);
    runN(2); CHECK(eng().undoDepth(0) == 0);                          // 400 ms after the release
}
static void doubleTapStops() {
    fresh(); recordBaseAndDub(); tapC(); runN(29);
    set(kParamUndoRedo, 1); runN(1); CHECK(eng().transport() == ouro::Transport::Stopping);   // on the press
    runN(20); set(kParamUndoRedo, 0); runN(200);
    CHECK(eng().transport() == ouro::Transport::Stopped && eng().undoDepth(0) == 1 && peak(gOut) == 0.0f);
}
static void bRestartsAfterStop() {
    fresh(); recordBaseAndDub(); doubleTap(); runN(20);
    set(kParamRec, 1); runN(1); set(kParamRec, 0);
    CHECK(eng().transport() == ouro::Transport::Running && eng().state(0) == ouro::TrackState::Playing);
}
static void holdWhileStoppedClearsAll() {
    fresh(); recordBaseAndDub(); tapC(); runN(152); CHECK(eng().canRedo(0));
    doubleTap(); runN(20);
    set(kParamUndoRedo, 1); runN(300); set(kParamUndoRedo, 0); runN(120);   // 800 ms: no redo, no undo
    CHECK(eng().undoDepth(0) == 0 && eng().loopLength() > 0 && eng().transport() == ouro::Transport::Stopped);
    set(kParamUndoRedo, 1); runN(376);                                        // past 1000 ms
    CHECK(eng().loopLength() == 0 && eng().transport() == ouro::Transport::Running);
    set(kParamUndoRedo, 0); runN(120); CHECK(eng().state(0) == ouro::TrackState::Empty);
}
static void doubleTapThenHoldDoesNotClear() {
    fresh(); recordBaseAndDub(); tapC(); runN(29); set(kParamUndoRedo, 1); runN(450);
    CHECK(eng().transport() == ouro::Transport::Stopped && eng().loopLength() > 0);
}
static void tapWhileStoppedUndoes() {
    fresh(); recordBaseAndDub(); doubleTap(); runN(20); tapC(); runN(152);
    CHECK(eng().undoDepth(0) == 0 && eng().transport() == ouro::Transport::Stopped);
}
static void bAfterATapUndoesFirst() {                                 // Ruling R8
    fresh(); recordBaseAndDub(); tapC(); runN(74);                    // ~200 ms after the release
    set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(120);         // re-dub: the bad layer goes first
    CHECK(eng().undoDepth(0) == 0 && eng().state(0) == ouro::TrackState::Overdubbing);
    fresh(); set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(37); // ~100 ms of a first recording
    tapC(); runN(55);                                                 // abort it, then ~150 ms later...
    set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(120);         // ...record again
    CHECK(eng().state(0) == ouro::TrackState::Recording && eng().loopLength() == 0);
}
static void doubleTapWhileStoppedResumes() {
    fresh(); recordBaseAndDub();
    set(kParamRec, 1); runN(1); set(kParamRec, 0); run(P(), sine(220, 0.1f, 4800 - 128));
    set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(40);           // a second layer
    doubleTap(); runN(20);
    CHECK(eng().transport() == ouro::Transport::Stopped && eng().undoDepth(0) == 2);
    set(kParamUndoRedo, 1); runN(2); set(kParamUndoRedo, 0); runN(2);  // two quick taps: resume from the top
    set(kParamUndoRedo, 1); runN(1);
    CHECK(eng().transport() == ouro::Transport::Running);              // on the second press
    runN(600); set(kParamUndoRedo, 0); runN(120);                       // held past 2x hold time: no clear
    CHECK(eng().undoDepth(0) == 2 && eng().loopLength() > 0 && eng().transport() == ouro::Transport::Running);
    doubleTap(); runN(20);                                              // slow taps while stopped each undo
    tapC(); runN(152); tapC(); runN(152);
    CHECK(eng().undoDepth(0) == 0 && eng().transport() == ouro::Transport::Stopped);
}
static void doubleTapBegunWhileStoppingOnlyStops() {                   // first tap during a fade, second after the park
    fresh(); recordBaseAndDub(); set(kParamStopMode, 2); set(kParamFadeTime, 0.5f);
    set(kParamStopPlay, 1); runN(1); set(kParamStopPlay, 0); runN(170);   // 171 blocks into a 188-block fade
    set(kParamUndoRedo, 1); runN(2); set(kParamUndoRedo, 0); runN(1);    // released while still stopping
    CHECK(eng().transport() == ouro::Transport::Stopping);
    runN(20); CHECK(eng().transport() == ouro::Transport::Stopped);
    set(kParamUndoRedo, 1); runN(1); set(kParamUndoRedo, 0); runN(120);
    CHECK(eng().transport() == ouro::Transport::Stopped && eng().undoDepth(0) == 1);
}
static void tapUndoesTheTrackItWasTappedOn() {                        // Review Focus 5
    fresh(); recordBaseAndDub();
    set(kParamTrack, 2); runN(1); set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(20);
    set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(40);           // track 2 recorded onto the loop
    set(kParamTrack, 1); runN(1); tapC(); set(kParamTrack, 2); runN(152);
    CHECK(eng().undoDepth(0) == 0 && eng().state(1) == ouro::TrackState::Playing);
}
static void doubleTapDuringAShortFirstRecording() {                   // Review Focus 4
    fresh(); set(kParamRec, 1); runN(1); set(kParamRec, 0); runN(5);
    set(kParamUndoRedo, 1); runN(2); set(kParamUndoRedo, 0); runN(2); set(kParamUndoRedo, 1); runN(1);
    CHECK(eng().state(0) == ouro::TrackState::Empty && eng().loopLength() == 0
          && eng().transport() == ouro::Transport::Running);
    set(kParamUndoRedo, 0); runN(200); CHECK(eng().state(0) == ouro::TrackState::Empty);
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
    for (int dryThru = 0; dryThru < 2; ++dryThru)
    {
        fresh();
        set(kParamDryThru, static_cast<float>(dryThru));
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
            const float want = dryThru ? s[pos - (out.size() - i)] : 0.0f;
            CHECK(std::fabs(out[i] - want) < 1e-6f);
        }
        set(kParamBypass, 0); feed(4800);
        CHECK(maxStep(slice(out, from - 1, out.size())) < limit);
        CHECK(peak(slice(out, out.size() - 2400, out.size())) > 0.05f);
    }
}

static void savedValuesSnapOnLoad()                               // Review Focus 2
{
    fresh(); set(kParamDryThru, 1); P().activate();
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
    set(kParamDryThru, 1); P().activate();
    auto s = sine(220, 0.1f, 512); run(P(), s);
    CHECK(maxAbsDiff(gOut, s) < 1e-7f);
    set(kParamRec, 1); runN(1); CHECK(eng().state(0) == ouro::TrackState::Empty);
    set(kParamDryThru, 0); P().deactivate(); P().activate(); run(P(), s); CHECK(peak(gOut) == 0.0f);
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
// A tap on C cancels a waiting action on its press, not when the double-tap window ends: a late undo would act on
// what the line did (here undo the first loop it closed, and the take is lost). A second tap still stops, and B
// after the tap acts without an undo first.
static void runTo(double t) { while (gT < t) runMod(true, 1); }
static void waitingClose() {                                        // a first recording from the line; B early for bar 3
    fresh(); gT = 0; set(kParamSync, 1); runMod(true, 1);
    set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0);
    runTo(192000 - 40000); set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0);
    CHECK(eng().userWaiting(0));
}
static void tapCancelsAWaitingActionAtOnce() {
    for (double before : {14400.0, 1280.0}) {                       // 300 ms before the line; 27 ms (released after it)
        waitingClose(); runTo(192000 - before);
        set(kParamUndoRedo, 1); runMod(true, 1); CHECK(!eng().waiting() && eng().state(0) == ouro::TrackState::Recording);
        runMod(true, 19); set(kParamUndoRedo, 0); runMod(true, 200);  // a 53 ms tap; the line and the window pass
        CHECK(eng().state(0) == ouro::TrackState::Recording && eng().loopLength() == 0 && !eng().waiting());
        CHECK(eng().recordedSamples() > 192000);                    // still recording
    }
    waitingClose(); runTo(192000 - 14400);                          // a double-tap stops: whole bars kept
    set(kParamUndoRedo, 1); runMod(true, 2); set(kParamUndoRedo, 0); runMod(true, 2);
    set(kParamUndoRedo, 1); runMod(true, 1); set(kParamUndoRedo, 0); runMod(true, 40);
    CHECK(eng().transport() == ouro::Transport::Stopped && eng().loopLength() == 96000);
    waitingClose(); runTo(192000 - 30000);                          // B within the window: no undo, it closes again
    set(kParamUndoRedo, 1); runMod(true, 2); set(kParamUndoRedo, 0); runMod(true, 20);
    set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0);
    CHECK(eng().state(0) == ouro::TrackState::Recording && eng().userWaiting(0));
    runTo(192000 + 128 * 200); CHECK(eng().loopLength() == 192000 && eng().state(0) == ouro::TrackState::Playing);
}
// The tap cancels a wait on any track at once: B waits on track 1, the knob turns to track 2 (its loop), C just before
// the line. Track 1 does not start and track 2 is not undone.
static void tapCancelsAWaitOnAnotherTrack() {
    fresh(); gT = 0; set(kParamSync, 1); set(kParamTrack, 2); runMod(true, 1);
    set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0);           // track 2's first loop: one bar
    runTo(96000 - 1280); set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0); runTo(96000 + 128 * 20);
    CHECK(eng().loopLength() == 96000 && eng().state(1) == ouro::TrackState::Playing);
    set(kParamTrack, 1); runMod(true, 1);
    runTo(2 * 96000 + 30000); set(kParamRec, 1); runMod(true, 1); set(kParamRec, 0);   // track 1 waits for the line
    CHECK(eng().userWaiting(0) && eng().waitingTrack() == 0);
    set(kParamTrack, 2); runMod(true, 1); runTo(3 * 96000 - 1280);
    set(kParamUndoRedo, 1); runMod(true, 1); CHECK(!eng().waiting());
    runMod(true, 19); set(kParamUndoRedo, 0); runMod(true, 200);
    CHECK(eng().state(0) == ouro::TrackState::Empty && !eng().waiting());
    CHECK(eng().state(1) == ouro::TrackState::Playing && !eng().canRedo(1));
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

int main()
{
    d_nextBufferSize = 128;
    d_nextSampleRate = 48000.0;
    d_nextCanRequestParameterValueChanges = true;

    portSymbolsAreFrozen();
    silentAndMono();
    recActsOnRisingEdgeOnly();
    tapUndoHoldRedo();
    tapUndoWaitsForTheWindow();
    doubleTapStops();
    bRestartsAfterStop();
    holdWhileStoppedClearsAll();
    doubleTapThenHoldDoesNotClear();
    tapWhileStoppedUndoes();
    doubleTapWhileStoppedResumes();
    doubleTapBegunWhileStoppingOnlyStops();
    tapUndoesTheTrackItWasTappedOn();
    bAfterATapUndoesFirst();
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
    tapCancelsAWaitingActionAtOnce();
    tapCancelsAWaitOnAnotherTrack();
    clickTicksOnModsBeats();
    clickIsSilentUnlessOnSyncedAndPlaying();
    lengthReachesTheEngine();
    REPORT();
}
