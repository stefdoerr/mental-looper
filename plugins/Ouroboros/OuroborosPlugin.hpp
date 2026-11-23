/*
 * Ouroboros — 4-track looper LV2 plugin for the MOD Dwarf.
 * Mono in / mono out. The plugin maps ports and footswitch gestures onto the
 * engine (dsp/Looper.hpp); it makes no DSP decisions of its own.
 */
#pragma once

#include "DistrhoPlugin.hpp"
#include "hmi/alabs-hmi-widgets.h"
#include "dsp/Looper.hpp"
#include "hmi/HmiBridge.hpp"
#include "hmi/LoopWidget.hpp"
#include "hmi/lv2-hmi.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>

START_NAMESPACE_DISTRHO

// Flush denormals on the audio thread (denormals cost ~100x on x86 and cause xruns).
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  #include <xmmintrin.h>
  #include <pmmintrin.h>
  static inline void flushDenormals() {
      _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
      _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
  }
#elif defined(__aarch64__)
  static inline void flushDenormals() {
      uint64_t fpcr; __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
      __asm__ __volatile__("msr fpcr, %0" :: "r"(fpcr | (1ULL << 24)));  // FZ bit
  }
#else
  static inline void flushDenormals() {}
#endif

static_assert(!(DISTRHO_PLUGIN_WANT_STATE || DISTRHO_PLUGIN_WANT_MIDI_INPUT || DISTRHO_PLUGIN_WANT_MIDI_OUTPUT
                || DISTRHO_PLUGIN_WANT_TIMEPOS || DISTRHO_PLUGIN_WANT_LATENCY),
              "paramFromLv2Index assumes no LV2 event or latency ports");

// LV2 port index -> parameter index. Valid because the plugin has no event
// ports (no state, MIDI or time position): params follow the audio ports.
constexpr uint32_t paramFromLv2Index(uint32_t lv2Index)
{
    return lv2Index - (DISTRHO_PLUGIN_NUM_INPUTS + DISTRHO_PLUGIN_NUM_OUTPUTS);
}

// Port order and symbols are frozen from the first release (saved pedalboards).
enum ParamIndex {
    kParamRec = 0, kParamUndoRedo, kParamTrack, kParamOnOff, kParamVolume,
    kParamUndo, kParamRedo, kParamClearTrack, kParamClearAll,
    kParamOn1, kParamOn2, kParamOn3, kParamOn4,
    kParamVolume1, kParamVolume2, kParamVolume3, kParamVolume4,
    kParamAfterRec, kParamDryThru, kParamTail, kParamHoldTime, kParamBudget,
    kParamOffset, kParamTriggerLatency,
    kParamStopPlay, kParamStopMode, kParamFadeTime, kParamAutoRec, kParamThreshold, kParamDoubleTap,
    kParamBypass,
    kNumParams
};

enum GroupId { kGroupPerform = 0, kGroupTracks, kGroupSettings };

class OuroborosPlugin : public Plugin
{
public:
    OuroborosPlugin() : Plugin(kNumParams, 0, 0)
    {
        for (uint32_t i = 0; i < kNumParams; ++i)
        {
            Parameter p;
            initParameter(i, p);
            fVal[i] = p.ranges.def;
        }
        fEngine.init(getSampleRate(), getBufferSize());   // the only allocation: never in run()
        // LV2 only (null for VST3/CLAP): the host's optional HMI features.
        if (const auto* f = static_cast<const LV2_Feature* const*>(d_nextLv2Features))
            for (; *f != nullptr; ++f)
            {
                if (std::strcmp((*f)->URI, LV2_HMI__WidgetControl) == 0)
                    fHmi.setFeature(static_cast<const LV2_HMI_WidgetControl*>((*f)->data));
                else if (std::strcmp((*f)->URI, ALABS_HMI__WidgetDraw) == 0)
                    fWidgetDraw = static_cast<const ALABS_HMI_WidgetDraw*>((*f)->data);
            }
    }

    const ouro::Looper& engine() const { return fEngine; }
    int selectedTrack() const { return fSel; }   // 0-3

    // LV2 extension callbacks (OuroborosPlugin.cpp), from non-RT host threads,
    // possibly while run() runs: share state with run() through atomics only.
    void hmiAddressed(uint32_t lv2Index, LV2_HMI_Addressing a, const LV2_HMI_AddressingInfo* info)
    {
        int p = hmiPort(lv2Index);
        if (p >= 0 && info != nullptr) fHmi.addressed(ouro::HmiPort(p), a, uint32_t(info->caps));
    }
    void hmiUnaddressed(uint32_t lv2Index)
    {
        int p = hmiPort(lv2Index);
        if (p >= 0) fHmi.unaddressed(ouro::HmiPort(p));
    }

    void widgetShown(const char* symbol, ALABS_HMI_Widget w, const ALABS_HMI_WidgetArea* area)
    {
        if (fWidgetDraw == nullptr || w == nullptr || area == nullptr || std::strcmp(symbol, "loop") != 0
            || area->pixelFormat != ALABS_HMI_PixelFormat_Mono1
            || area->width == 0 || area->width > kMaxW || area->height == 0 || area->height > kMaxH)
            return;
        // Take a free slot, write its size, publish the handle last. Every showing gets a new
        // handle, so run() redraws the whole area whenever the handle differs from the one it drew.
        for (auto& s : fSlots)
            if (s.handle.load(std::memory_order_acquire) == nullptr)
            {
                s.w.store(area->width, std::memory_order_relaxed);
                s.h.store(area->height, std::memory_order_relaxed);
                s.handle.store(w, std::memory_order_release);
                return;
            }
    }

    void widgetHidden(ALABS_HMI_Widget w)
    {
        // Null the handle, then wait out a run() that loaded it just before: when this returns,
        // no draw_bitmap for w is running or will start. run() holds `drawing` only for one render+draw.
        for (auto& s : fSlots)
        {
            ALABS_HMI_Widget expected = w;
            if (w != nullptr && s.handle.compare_exchange_strong(expected, nullptr))
                while (s.drawing.load()) {}              // spin: std::this_thread is missing on MinGW win32-threads
        }
    }

protected:
    static int hmiPort(uint32_t lv2Index)
    {
        if (lv2Index < DISTRHO_PLUGIN_NUM_INPUTS + DISTRHO_PLUGIN_NUM_OUTPUTS) return -1;
        switch (paramFromLv2Index(lv2Index)) {
        case kParamRec:      return ouro::kHmiRec;
        case kParamUndoRedo: return ouro::kHmiUndo;
        case kParamOnOff:    return ouro::kHmiOnOff;
        case kParamVolume:   return ouro::kHmiVolume;
        default:             return -1;
        }
    }

    const char* getLabel()       const override { return DISTRHO_PLUGIN_NAME; }
    const char* getMaker()       const override { return DISTRHO_PLUGIN_BRAND; }
    const char* getHomePage()    const override { return PLUGIN_HOMEPAGE; }
    const char* getLicense()     const override { return "ISC"; }
    uint32_t    getVersion()     const override
    {
        // Injected by the Makefile from the top-level VERSION file.
#ifdef PLUGIN_VERSION_MAJOR
        return d_version(PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR, PLUGIN_VERSION_MICRO);
#else
        return d_version(0, 0, 0);
#endif
    }
    const char* getDescription() const override { return "A 4-track looper."; }

    int64_t getUniqueId() const override
    {
        // Must match DISTRHO_PLUGIN_UNIQUE_ID in DistrhoPluginInfo.h.
#ifdef OUROBOROS_BETA
        return d_cconst('d', 'O', 'u', 'B');
#else
        return d_cconst('d', 'O', 'u', 'r');
#endif
    }

    void initAudioPort(bool input, uint32_t index, AudioPort& port) override
    {
        port.groupId = kPortGroupMono;
        Plugin::initAudioPort(input, index, port);
    }

    void initPortGroup(uint32_t groupId, PortGroup& pg) override
    {
        // The gN_ prefixes pin the band order in MOD (alphabetical by symbol).
        switch (groupId)
        {
        case kGroupPerform:  pg.name = "Perform";  pg.symbol = "g1_perform";  break;
        case kGroupTracks:   pg.name = "Tracks";   pg.symbol = "g2_tracks";   break;
        case kGroupSettings: pg.name = "Settings"; pg.symbol = "g3_settings"; break;
        default: Plugin::initPortGroup(groupId, pg); break;
        }
    }

    void initParameter(uint32_t index, Parameter& p) override
    {
        if (index == kParamBypass)
        {
            p.initDesignation(kParameterDesignationBypass);
            return;
        }
        enum { Trig, Bool, Int, Enum, Db, Ms };
        struct Def { const char* symbol; const char* name; int kind; float min, max, def; const char* unit; };
        static const Def defs[kParamBypass] = {
            {"rec",             "Rec",                Trig, 0, 1, 0, ""},
            {"undo_redo",       "Undo/Redo",          Bool, 0, 1, 0, ""},
            {"track",           "Track",              Enum, 1, 4, 1, ""},
            {"onoff",           "On/Off",             Trig, 0, 1, 0, ""},
            {"volume",          "Volume",             Db,  -60, 6, 0, "dB"},
            {"undo",            "Undo",               Trig, 0, 1, 0, ""},
            {"redo",            "Redo",               Trig, 0, 1, 0, ""},
            {"clear_track",     "Clear Track",        Trig, 0, 1, 0, ""},
            {"clear_all",       "Clear All",          Trig, 0, 1, 0, ""},
            {"on_1",            "On 1",               Bool, 0, 1, 1, ""},
            {"on_2",            "On 2",               Bool, 0, 1, 1, ""},
            {"on_3",            "On 3",               Bool, 0, 1, 1, ""},
            {"on_4",            "On 4",               Bool, 0, 1, 1, ""},
            {"volume_1",        "Volume 1",           Db,  -60, 6, 0, "dB"},
            {"volume_2",        "Volume 2",           Db,  -60, 6, 0, "dB"},
            {"volume_3",        "Volume 3",           Db,  -60, 6, 0, "dB"},
            {"volume_4",        "Volume 4",           Db,  -60, 6, 0, "dB"},
            {"after_rec",       "After Rec",          Enum, 0, 1, 0, ""},
            {"dry_thru",        "Dry Thru",           Bool, 0, 1, 0, ""},
            {"tail",            "Tail",               Ms,   0, 500, 50, "ms"},
            {"hold_time",       "Hold Time",          Ms, 200, 2000, 500, "ms"},
            {"budget",          "Undo Budget",        Ms,  16, 400, 200, "MB"},
            {"offset",          "Overdub earlier by", Ms,   0, 30, 0, "ms"},
            {"trigger_latency", "Trigger Latency",    Ms,   0, 50, 3, "ms"},
            {"stop_play",       "Stop/Play",          Trig, 0, 1, 0, ""},
            {"stop_mode",       "Stop Mode",          Enum, 0, 2, 0, ""},
            {"fade_time",       "Fade Time",          Ms, 0.5f, 10, 3, "s"},
            {"auto_rec",        "Auto Rec",           Bool, 0, 1, 0, ""},
            {"threshold",       "Threshold",          Db, -60, -10, -40, "dB"},
            {"double_tap",      "Double-Tap",         Ms, 150, 600, 300, "ms"},
        };
        const Def& d = defs[index];
        p.symbol = d.symbol;
        p.name = d.name;
        p.unit = d.unit;
        p.ranges.min = d.min; p.ranges.max = d.max; p.ranges.def = d.def;
        p.groupId = index <= kParamClearAll || index == kParamStopPlay ? kGroupPerform : index <= kParamVolume4 ? kGroupTracks : kGroupSettings;
        p.hints = kParameterIsAutomatable;
        switch (d.kind)
        {
        case Trig: p.hints |= kParameterIsTrigger | kParameterIsInteger; break;   // includes boolean
        case Bool: p.hints |= kParameterIsBoolean | kParameterIsInteger; break;
        case Int:  p.hints |= kParameterIsInteger; break;
        case Enum:
        {
            const uint8_t n = index == kParamTrack ? 4 : index == kParamStopMode ? 3 : 2;
            static const char* const trackNames[] = {"Track 1", "Track 2", "Track 3", "Track 4"};
            static const char* const afterNames[] = {"Rec > Play", "Rec > Dub"};
            static const char* const stopNames[] = {"Instant", "End of loop", "Fade"};
            p.hints |= kParameterIsInteger;
            p.enumValues.count = n;
            p.enumValues.restrictedMode = true;
            p.enumValues.values = new ParameterEnumerationValue[n];
            for (uint8_t k = 0; k < n; ++k)
            {
                p.enumValues.values[k].value = d.min + k;
                p.enumValues.values[k].label = index == kParamTrack ? trackNames[k] : index == kParamStopMode ? stopNames[k] : afterNames[k];
            }
            break;
        }
        default: break;
        }
    }

    float getParameterValue(uint32_t index) const override { return fVal[index]; }

    void setParameterValue(uint32_t index, float value) override { fVal[index] = value; }

    void activate() override
    {
        fEngine.reset();
        fFirst = true;
        fHold = 0;
        fHeld = fDone = fTapPending = fStoppedPress = false;
        fSinceTap = 0; fTapTrack = 0;
    }

    void sampleRateChanged(double sr) override
    {
        fEngine.init(sr, getBufferSize());      // non-RT; re-reserves for the new rate
        fFirst = true;                          // the fresh engine takes every value again
    }

    void run(const float** inputs, float** outputs, uint32_t frames) override
    {
        flushDenormals();

        const auto on = [this](uint32_t i) { return fVal[i] > 0.5f; };
        const auto request = [this](uint32_t i, float v) { fVal[i] = v; fPrev[i] = v; requestParameterValueChange(i, v); };

        // Track selection first: it retargets everything below.
        const int sel = std::min(3, std::max(0, int(std::lround(fVal[kParamTrack])) - 1));
        const bool trackChanged = sel != fSel;
        fSel = sel;
        if (fFirst)
            for (uint32_t i = 0; i < kParamBypass; ++i) fPrev[i] = std::nanf("");   // every value counts as changed
        const auto changed = [this](uint32_t i) { return fVal[i] != fPrev[i]; };

        // Settings and per-track ports: drive the engine on change (all of them on the first run).
        for (int t = 0; t < 4; ++t)
        {
            if (changed(kParamOn1 + t)) fEngine.setOn(t, on(kParamOn1 + t));
            if (changed(kParamVolume1 + t))
            {
                fEngine.setVolumeDb(t, fVal[kParamVolume1 + t]);
                if (t == sel && !changed(kParamVolume)) request(kParamVolume, fVal[kParamVolume1 + t]);
            }
        }
        if (changed(kParamAfterRec))      fEngine.setRecToDub(on(kParamAfterRec));
        if (changed(kParamDryThru))       fEngine.setDryThru(on(kParamDryThru));
        if (changed(kParamTail))          fEngine.setTailMs(fVal[kParamTail]);
        if (changed(kParamBudget))        fEngine.setBudgetMb(fVal[kParamBudget]);
        if (changed(kParamOffset))        fEngine.setAdvanceMs(fVal[kParamOffset]);
        if (changed(kParamTriggerLatency)) fEngine.setTriggerLatencyMs(fVal[kParamTriggerLatency]);
        if (changed(kParamAutoRec))       fEngine.setAutoRec(on(kParamAutoRec));
        if (changed(kParamThreshold))     fEngine.setThresholdDb(fVal[kParamThreshold]);
        fEngine.setBypass(on(kParamBypass));

        // Volume proxy: follows the selected track; a different value written to it goes to that track.
        const uint32_t vn = kParamVolume1 + sel;
        if (trackChanged || (fFirst && fVal[kParamVolume] != fVal[vn]))
            request(kParamVolume, fVal[vn]);
        else if (changed(kParamVolume) && fVal[kParamVolume] != fVal[vn])
        {
            fEngine.setVolumeDb(sel, fVal[kParamVolume]);
            request(vn, fVal[kParamVolume]);
        }

        // Triggers and the footswitch act on rising edges only; a trigger held high from the
        // start (previous value NaN on the first run) counts as a press.
        const auto rising = [&](uint32_t i) { return on(i) && !(fPrev[i] > 0.5f); };
        if (rising(kParamRec))        fEngine.pressRec(sel);
        if (rising(kParamUndo))       fEngine.undo(sel);
        if (rising(kParamRedo))       fEngine.redo(sel);
        if (rising(kParamClearTrack)) fEngine.clearTrack(sel);
        if (rising(kParamClearAll))   fEngine.clearAll();
        if (rising(kParamStopPlay))
        {
            if (fEngine.transport() == ouro::Transport::Running) stopNow(); else fEngine.restart();
        }
        if (rising(kParamOnOff))
        {
            fEngine.toggleOn(sel);
            request(kParamOn1 + sel, fEngine.isOn(sel) ? 1.0f : 0.0f);
        }

        // C footswitch: a tap undoes once the double-tap window passes, a second tap within it stops,
        // a hold of hold_time redoes once, and a hold of 2x hold_time while stopped clears all.
        // ponytail: timed in whole blocks (<= one block of error); sample-exact if it matters.
        const uint64_t hold = uint64_t(fVal[kParamHoldTime] * 0.001 * getSampleRate());
        const uint64_t window = uint64_t(fVal[kParamDoubleTap] * 0.001 * getSampleRate());
        if (on(kParamUndoRedo))
        {
            if (!fHeld)
            {
                fHeld = true; fHold = 0; fDone = false;
                fStoppedPress = fEngine.transport() == ouro::Transport::Stopped;
                if (fTapPending) { fTapPending = false; fDone = true; fStoppedPress = false; stopNow(); }
            }
            fHold += frames;
            if (!fDone && !fStoppedPress && fHold >= hold) { fDone = true; fEngine.redo(sel); }
            if (!fDone && fStoppedPress && fHold >= 2 * hold) { fDone = true; fEngine.clearAll(); }
        }
        else if (fHeld)
        {
            fHeld = false;
            if (!fDone && fHold < hold) { fTapPending = true; fSinceTap = 0; fTapTrack = sel; }
        }
        else if (fTapPending && (fSinceTap += frames) >= window)
        {
            fTapPending = false;
            fEngine.undo(fTapTrack);
        }

        for (uint32_t i = 0; i < kParamBypass; ++i) fPrev[i] = fVal[i];
        const bool fWasFirst = fFirst;
        fFirst = false;

        if (fWasFirst) fEngine.snapGains();
        fEngine.process(inputs[0], outputs[0], frames);

        // Widgets: blink toggles every 250 ms of audio; slots are redrawn every 960 samples (20 ms at 48 kHz).
        fBlinkAcc += frames;
        const uint32_t blinkPeriod = uint32_t(getSampleRate() * 0.25);
        if (fBlinkAcc >= blinkPeriod) { fBlinkAcc = 0; fBlinkOn = !fBlinkOn; }
        fDrawAcc += frames;
        if (fDrawAcc >= 960) { fDrawAcc %= 960; drawWidgets(); }

        fHmi.update({fEngine.state(sel), fEngine.nextAction(sel), fEngine.canUndo(sel), sel, fEngine.isOn(sel)});
    }

private:
    void stopNow()
    {
        const int m = std::min(2, std::max(0, int(std::lround(fVal[kParamStopMode]))));
        fEngine.stop(ouro::StopMode(m), fVal[kParamFadeTime] * 1000.0f);
    }

    // Renders each shown slot and draws it when the bitmap changed or the handle is new. No allocation: fixed buffers.
    void drawWidgets()
    {
        if (fWidgetDraw == nullptr) return;
        ouro::WidgetView v = {};
        v.selected = fSel;
        for (int t = 0; t < ouro::kTracks; ++t) { v.state[t] = fEngine.state(t); v.on[t] = fEngine.isOn(t); }
        v.loopLength = fEngine.loopLength(); v.position = fEngine.position();
        v.recordedSamples = fEngine.recordedSamples(); v.sampleRate = fEngine.sampleRate(); v.blinkOn = fBlinkOn;

        uint8_t bits[kMaxBytes];
        for (auto& s : fSlots)
        {
            s.drawing.store(true);                       // seq_cst: pairs with widgetHidden's null store + drawing load
            const ALABS_HMI_Widget w = s.handle.load();
            if (w != nullptr)
            {
                const uint16_t sw = s.w.load(std::memory_order_relaxed), sh = s.h.load(std::memory_order_relaxed);
                const size_t n = ouro::renderLoopWidget(v, sw, sh, bits);
                if (w != s.drawn || std::memcmp(bits, s.last, n) != 0)
                {
                    fWidgetDraw->draw_bitmap(fWidgetDraw->handle, w, 0, 0, sw, sh, bits);
                    std::memcpy(s.last, bits, n);
                }
            }
            s.drawn = w;
            s.drawing.store(false);
        }
    }

    ouro::Looper fEngine;
    float fVal[kNumParams] = {};            // current port values
    float fPrev[kParamBypass] = {};         // values as of the last run() (NaN: apply everything)
    int fSel = 0;                           // selected track, 0-3
    bool fFirst = true;                     // first run after activate(): apply every value, then snap gains
    uint64_t fHold = 0;                     // samples undo_redo has been held
    bool fHeld = false, fDone = false, fTapPending = false, fStoppedPress = false;
    uint64_t fSinceTap = 0;
    int fTapTrack = 0;

    ouro::HmiBridge fHmi;                           // labels and LEDs; inert without the host's HMI
    const ALABS_HMI_WidgetDraw* fWidgetDraw = nullptr;  // null: host can't show widgets

    static constexpr uint16_t kMaxW = 128, kMaxH = 64;
    static constexpr size_t kMaxBytes = (kMaxW / 8) * kMaxH;   // 1024

    // One shown widget; handle is null while the slot is free.
    struct WidgetSlot {
        std::atomic<ALABS_HMI_Widget> handle{nullptr};
        std::atomic<uint16_t> w{0}, h{0};
        std::atomic<bool> drawing{false};   // run() is between loading the handle and finishing draw_bitmap
        ALABS_HMI_Widget drawn = nullptr;   // run() only: the handle last drawn (new handle = redraw all)
        uint8_t last[kMaxBytes] = {};       // run() only: the last bitmap drawn
    } fSlots[4];
    uint32_t fDrawAcc = 0, fBlinkAcc = 0;   // samples since the last widget redraw / blink toggle
    bool fBlinkOn = false;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OuroborosPlugin)
};

END_NAMESPACE_DISTRHO
