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
static std::vector<float> gOut;
static void run(PluginExporter& plugin, const std::vector<float>& vec)
{
    gOut.assign(vec.size(), 0.777f);
    for (size_t i = 0; i < vec.size(); i += 128)
    {
        const uint32_t len = static_cast<uint32_t>(std::min<size_t>(128, vec.size() - i));
        const float* inputs[1]  = { vec.data() + i };
        float*       outputs[1] = { gOut.data() + i };
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
        "dry_thru","tail","hold_time","budget","offset","trigger_latency" };
    for (uint32_t i = 0; i < 24; ++i) CHECK(std::strcmp(P().getParameterSymbol(i), kSym[i]) == 0);
    CHECK(P().getParameterDesignation(kParamBypass) == kParameterDesignationBypass);
    CHECK(kParamBypass == 24 && P().getParameterCount() == 25);
    CHECK(std::strcmp(P().getLabel(), DISTRHO_PLUGIN_NAME) == 0);
    // groups: g1_perform 0-8, g2_tracks 9-16, g3_settings 17-23
    for (uint32_t i = 0; i < 24; ++i)
    {
        const uint32_t g = P().getParameterGroupId(i);
        CHECK(g == (i <= 8 ? 0u : i <= 16 ? 1u : 2u));
    }
    CHECK(std::strcmp(P().getPortGroupById(0).symbol, "g1_perform") == 0);
    CHECK(std::strcmp(P().getPortGroupById(1).symbol, "g2_tracks") == 0);
    CHECK(std::strcmp(P().getPortGroupById(2).symbol, "g3_settings") == 0);
    CHECK(P().isParameterTrigger(kParamRec) && !P().isParameterTrigger(kParamUndoRedo));
    CHECK(P().getParameterRanges(kParamVolume).def == 0.0f && P().getParameterRanges(kParamVolume).min == -60.0f);
    CHECK(P().getParameterRanges(kParamTail).def == 50.0f && P().getParameterRanges(kParamHoldTime).def == 500.0f);
    CHECK(P().getParameterRanges(kParamBudget).min == 16.0f && P().getParameterRanges(kParamBudget).def == 200.0f);
    CHECK(P().getParameterRanges(kParamTriggerLatency).def == 3.0f && P().getParameterRanges(kParamOn1).def == 1.0f);
}

static void silentAndMono()
{
    fresh();
    CHECK(DISTRHO_PLUGIN_NUM_INPUTS == 1 && DISTRHO_PLUGIN_NUM_OUTPUTS == 1);
    P().activate();
    run(P(), sine(220, 0.1f, 4800));        // nothing recorded, dry_thru off -> silence
    CHECK(peak(gOut) == 0.0f);
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
    set(kParamUndoRedo, 1); runN(37); set(kParamUndoRedo, 0); runN(1);    // ~99 ms tap
    CHECK(eng().undoDepth(0) == 0 && eng().canRedo(0));
    set(kParamUndoRedo, 1); runN(190);                                     // ~507 ms held
    CHECK(eng().undoDepth(0) == 1);                                        // redo fired while held
    set(kParamUndoRedo, 0); runN(1); CHECK(eng().undoDepth(0) == 1);       // no undo on release
    // a hold_time of 2000 ms turns the same 507 ms hold into a plain undo on release
    set(kParamHoldTime, 2000); set(kParamUndoRedo, 1); runN(190); CHECK(eng().undoDepth(0) == 1);
    set(kParamUndoRedo, 0); runN(1); CHECK(eng().undoDepth(0) == 0);
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

int main()
{
    d_nextBufferSize = 128;
    d_nextSampleRate = 48000.0;
    d_nextCanRequestParameterValueChanges = true;

    portSymbolsAreFrozen();
    silentAndMono();
    recActsOnRisingEdgeOnly();
    tapUndoHoldRedo();
    undoRedoClearTriggers();
    trackSelectRetargets();
    volumeProxyFollowsSelectedTrack();
    onoffFlipsSelectedAndReportsPort();
    settingsReachTheEngine();
    bypassSemantics();
    savedValuesSnapOnLoad();
    reactivateResets();
    disabledEngineStillPassesDry();
    REPORT();
}
