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
#include "hmi/SongWidget.hpp"
#include "hmi/lv2-hmi.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
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
                || DISTRHO_PLUGIN_WANT_LATENCY),
              "paramFromLv2Index assumes no LV2 event port but time position's, and no latency port");

// LV2 index of the first parameter: after the audio ports and, with time position, DPF's events-in port
// (no state, MIDI or latency ports).
constexpr uint32_t kLv2FirstParam = DISTRHO_PLUGIN_NUM_INPUTS + DISTRHO_PLUGIN_NUM_OUTPUTS + DISTRHO_PLUGIN_WANT_TIMEPOS;

// LV2 port index -> parameter index.
constexpr uint32_t paramFromLv2Index(uint32_t lv2Index)
{
    return lv2Index - kLv2FirstParam;
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
    kParamSync, kParamClick, kParamClickLevel, kParamLength,
    kParamPart, kParamSong, kParamStep, kParamStepPart, kParamStepRepeats,
    // The song's steps, hidden: saved with the pedalboard (the Step proxies edit them).
    kParamStep1Part, kParamStep1Repeats, kParamStep2Part, kParamStep2Repeats, kParamStep3Part, kParamStep3Repeats,
    kParamStep4Part, kParamStep4Repeats, kParamStep5Part, kParamStep5Repeats, kParamStep6Part, kParamStep6Repeats,
    kParamStep7Part, kParamStep7Repeats, kParamStep8Part, kParamStep8Repeats, kParamStep9Part, kParamStep9Repeats,
    kParamStep10Part, kParamStep10Repeats, kParamStep11Part, kParamStep11Repeats, kParamStep12Part, kParamStep12Repeats,
    kParamStep13Part, kParamStep13Repeats, kParamStep14Part, kParamStep14Repeats, kParamStep15Part, kParamStep15Repeats,
    kParamStep16Part, kParamStep16Repeats,
    kParamBypass,
    kNumParams
};

enum GroupId { kGroupPerform = 0, kGroupTracks, kGroupSettings, kGroupSong };

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
        buildTicks(getSampleRate());
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
        const bool song = symbol != nullptr && std::strcmp(symbol, "song_steps") == 0;
        if (fWidgetDraw == nullptr || w == nullptr || area == nullptr || symbol == nullptr || (!song && std::strcmp(symbol, "loop") != 0)
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
                s.song.store(song, std::memory_order_relaxed);
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
        if (lv2Index < kLv2FirstParam) return -1;
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
        if (!input && index == 1)               // the click, on its own output (ttl symbol lv2_click_out)
        {
            port.name = "Click"; port.symbol = "click_out"; port.groupId = kPortGroupNone;
            return;
        }
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
        case kGroupSong:     pg.name = "Song";     pg.symbol = "g4_song";     break;
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
        if (index >= kParamStep1Part)           // step1_part, step1_repeats, ...: A x1, then Loop
        {
            const unsigned s = (index - kParamStep1Part) / 2 + 1;
            const bool reps = (index - kParamStep1Part) % 2;
            char sym[32], name[32];
            std::snprintf(sym, sizeof sym, reps ? "step%u_repeats" : "step%u_part", s);
            std::snprintf(name, sizeof name, reps ? "Step %u Repeats" : "Step %u Part", s);
            p.symbol = sym;
            p.name = name;
            p.ranges.min = reps ? 1 : 0; p.ranges.max = reps ? 16 : ouro::kStepStop;
            p.ranges.def = reps ? 1 : s == 1 ? 0 : ouro::kStepLoop;
            p.groupId = kGroupSong;
            p.hints = kParameterIsAutomatable | kParameterIsInteger | kParameterIsHidden;
            return;
        }
        enum { Trig, Bool, Int, Enum, Db, Ms };
        struct Def { const char* symbol; const char* name; int kind; float min, max, def; const char* unit; };
        static const Def defs[kParamStep1Part] = {
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
            {"double_tap",      "Double-Tap",         Ms, 150, 600, 400, "ms"},
            {"sync",            "Sync",               Enum, 0, 1, 0, ""},
            {"click",           "Click",              Bool, 0, 1, 0, ""},
            {"click_level",     "Click Level",        Db, -40, 0, -10, "dB"},
            {"length",          "Length",             Enum, 0, 16, 0, ""},
            {"part",            "Part",               Enum, 0, 3, 0, ""},
            {"song",            "Song",               Bool, 0, 1, 0, ""},
            {"step",            "Step",               Int,  1, 16, 1, ""},
            {"step_part",       "Step Part",          Enum, 0, 5, 0, ""},
            {"step_repeats",    "Step Repeats",       Int,  1, 16, 1, ""},
        };
        const Def& d = defs[index];
        p.symbol = d.symbol;
        p.name = d.name;
        p.unit = d.unit;
        p.ranges.min = d.min; p.ranges.max = d.max; p.ranges.def = d.def;
        p.groupId = index <= kParamClearAll || index == kParamStopPlay ? kGroupPerform
                  : index <= kParamVolume4 ? kGroupTracks : index >= kParamPart ? kGroupSong : kGroupSettings;
        p.hints = kParameterIsAutomatable;
        switch (d.kind)
        {
        case Trig: p.hints |= kParameterIsTrigger | kParameterIsInteger; break;   // includes boolean
        case Bool: p.hints |= kParameterIsBoolean | kParameterIsInteger; break;
        case Int:  p.hints |= kParameterIsInteger; break;
        case Enum:
        {
            const uint8_t n = index == kParamTrack || index == kParamPart ? 4 : index == kParamStopMode ? 3
                            : index == kParamLength ? 17 : index == kParamStepPart ? 6 : 2;
            static const char* const trackNames[] = {"Track 1", "Track 2", "Track 3", "Track 4"};
            static const char* const afterNames[] = {"Rec > Play", "Rec > Dub"};
            static const char* const stopNames[] = {"Instant", "End of loop", "Fade"};
            static const char* const syncNames[] = {"Off", "Bars"};
            static const char* const partNames[] = {"A", "B", "C", "D", "Loop", "Stop"};
            static const char* const lengthNames[] = {"Free", "1 bar", "2 bars", "3 bars", "4 bars", "5 bars", "6 bars",
                "7 bars", "8 bars", "9 bars", "10 bars", "11 bars", "12 bars", "13 bars", "14 bars", "15 bars", "16 bars"};
            p.hints |= kParameterIsInteger;
            p.enumValues.count = n;
            p.enumValues.restrictedMode = true;
            p.enumValues.values = new ParameterEnumerationValue[n];
            for (uint8_t k = 0; k < n; ++k)
            {
                p.enumValues.values[k].value = d.min + k;
                p.enumValues.values[k].label = index == kParamTrack ? trackNames[k] : index == kParamStopMode ? stopNames[k]
                                             : index == kParamSync ? syncNames[k] : index == kParamLength ? lengthNames[k]
                                             : index == kParamPart || index == kParamStepPart ? partNames[k] : afterNames[k];
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
        fHeld = fDone = fTapPending = fStoppedPress = fTapStopped = fTapCancels = false;
        fSinceTap = 0; fTapTrack = 0;
    }

    void sampleRateChanged(double sr) override
    {
        fEngine.init(sr, getBufferSize());      // non-RT; re-reserves for the new rate
        buildTicks(sr);
        fFirst = true;                          // the fresh engine takes every value again
    }

    void run(const float** inputs, float** outputs, uint32_t frames) override
    {
        flushDenormals();

        const auto on = [this](uint32_t i) { return fVal[i] > 0.5f; };
        const auto request = [this](uint32_t i, float v) { fVal[i] = v; fPrev[i] = v; requestParameterValueChange(i, v); };
        const auto num = [this](uint32_t i, int lo, int hi) { return std::min(hi, std::max(lo, int(std::lround(fVal[i])))); };

        // Track selection first: it retargets everything below.
        const int sel = num(kParamTrack, 1, 4) - 1;
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
        if (changed(kParamLength))        fEngine.setFixedBars(uint32_t(std::max(0L, std::lround(fVal[kParamLength]))));
        if (changed(kParamClickLevel))    fClickGain = std::pow(10.0f, fVal[kParamClickLevel] / 20.0f);
        if (changed(kParamStopMode) || changed(kParamFadeTime))   // what a Stop step uses
            fEngine.setStopMode(stopMode(), fVal[kParamFadeTime] * 1000.0f);
        fEngine.setBypass(on(kParamBypass));

        // Sync = Bars: MOD's bar grid, before the presses below so they see this block's grid, then its play/stop
        // edges. The first run and Sync turning on only take MOD's state: neither is an edge. Off: only the flag test.
        fClickAt = -1.0;                        // no tick this block unless followMod finds one
        if (changed(kParamSync)) fEngine.setSync(on(kParamSync));
        if (on(kParamSync)) followMod(changed(kParamSync), on(kParamClick));

        // Volume proxy: follows the selected track; a different value written to it goes to that track.
        const uint32_t vn = kParamVolume1 + sel;
        if (trackChanged || (fFirst && fVal[kParamVolume] != fVal[vn]))
            request(kParamVolume, fVal[vn]);
        else if (changed(kParamVolume) && fVal[kParamVolume] != fVal[vn])
        {
            fEngine.setVolumeDb(sel, fVal[kParamVolume]);
            request(vn, fVal[kParamVolume]);
        }

        // The song: the hidden step ports are the arrangement (every one on the first run, so a loaded song is in place
        // before Song comes on). Step Part and Step Repeats are proxies of the selected step's ports, as Volume is of
        // the selected track's. Part goes before Song: with both changed (a load, a preset) Song on still holds, and a
        // load with Song on starts in step 1's part.
        for (int s = 0; s < ouro::kSteps; ++s)
        {
            const uint32_t i = kParamStep1Part + 2 * s;
            if (changed(i) || changed(i + 1)) fEngine.setStep(s, num(i, 0, ouro::kStepStop), uint32_t(num(i + 1, 1, 16)));
        }
        const int step = num(kParamStep, 1, ouro::kSteps) - 1;
        const uint32_t sp = kParamStep1Part + 2 * step;
        bool edited = false;
        for (uint32_t k = 0; k < 2; ++k)
        {
            const uint32_t proxy = kParamStepPart + k;
            if (changed(kParamStep) || (changed(sp + k) && !changed(proxy))) request(proxy, fVal[sp + k]);
            else if (changed(proxy) && fVal[proxy] != fVal[sp + k]) { request(sp + k, fVal[proxy]); edited = true; }
        }
        if (edited) fEngine.setStep(step, num(sp, 0, ouro::kStepStop), uint32_t(num(sp + 1, 1, 16)));
        if (changed(kParamPart)) fEngine.setPart(num(kParamPart, 0, ouro::kParts - 1));
        if (changed(kParamSong)) fEngine.setSong(on(kParamSong));

        // Triggers and the footswitch act on rising edges only; a trigger held high from the
        // start (previous value NaN on the first run) counts as a press.
        const auto rising = [&](uint32_t i) { return on(i) && !(fPrev[i] > 0.5f); };
        if (fTapPending && rising(kParamRec))         // B after a tap: the undo first
        {
            fTapPending = false;
            if (!fTapCancels) fEngine.undo(fTapTrack);
        }
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

        // C footswitch: a tap undoes once the double-tap window passes (a wait for the line it cancels on its press);
        // a second tap within it stops, or, if the first tap was released while stopped, starts again from the top.
        // A hold of hold_time redoes once, and a hold of 2x hold_time while stopped clears all.
        // ponytail: timed in whole blocks (<= one block of error); sample-exact if it matters.
        const uint64_t hold = uint64_t(fVal[kParamHoldTime] * 0.001 * getSampleRate());
        const uint64_t window = uint64_t(fVal[kParamDoubleTap] * 0.001 * getSampleRate());
        if (on(kParamUndoRedo))
        {
            if (!fHeld)
            {
                fHeld = true; fHold = 0; fDone = false;
                fStoppedPress = fEngine.transport() == ouro::Transport::Stopped;
                if (fTapPending)                         // a double-tap: the pending undo is dropped
                {
                    fTapPending = false; fDone = true; fStoppedPress = false;
                    if (fTapStopped) fEngine.restart(); else stopNow();
                }
                // A wait for the line, on any track, is cancelled now: after the window the line may have acted.
                // The tap then undoes nothing, but a second tap still stops.
                else if ((fTapCancels = fEngine.userWaiting())) fEngine.undo(sel);
            }
            fHold += frames;
            if (!fDone && !fStoppedPress && fHold >= hold) { fDone = true; fEngine.redo(sel); }
            if (!fDone && fStoppedPress && fHold >= 2 * hold) { fDone = true; fEngine.clearAll(); }
        }
        else if (fHeld)
        {
            fHeld = false;
            if (!fDone && fHold < hold)
            {
                fTapPending = true; fSinceTap = 0; fTapTrack = sel;
                fTapStopped = fEngine.transport() == ouro::Transport::Stopped;
            }
        }
        else if (fTapPending && (fSinceTap += frames) >= window)
        {
            fTapPending = false;
            if (!fTapCancels) fEngine.undo(fTapTrack);
        }

        for (uint32_t i = 0; i < kParamBypass; ++i) fPrev[i] = fVal[i];
        const bool fWasFirst = fFirst;
        fFirst = false;

        if (fWasFirst) fEngine.snapGains();
        fEngine.process(inputs[0], outputs[0], frames);
        renderClick(outputs[1], frames);        // after the engine: an output may share the input's buffer

        // The engine moves Part (the song, Clear All) and Song (a hand on Part turns it off): the ports follow. Part
        // shows the part to play next, a queued one included. After a switch, On 1-4, Volume 1-4 and Volume show the
        // new part's tracks.
        const int next = fEngine.queuedPart() >= 0 ? fEngine.queuedPart() : fEngine.part();
        if (num(kParamPart, 0, ouro::kParts - 1) != next) request(kParamPart, float(next));
        if (on(kParamSong) != fEngine.songOn()) request(kParamSong, fEngine.songOn() ? 1.0f : 0.0f);
        if (fEngine.part() != fPart)
        {
            fPart = fEngine.part();
            for (int t = 0; t < 4; ++t)
            {
                request(kParamOn1 + t, fEngine.isOn(t) ? 1.0f : 0.0f);
                request(kParamVolume1 + t, fEngine.volumeDb(t));
            }
            request(kParamVolume, fEngine.volumeDb(sel));
        }

        // Widgets: blink toggles every 250 ms of audio; slots are redrawn every 960 samples (20 ms at 48 kHz).
        fBlinkAcc += frames;
        const uint32_t blinkPeriod = uint32_t(getSampleRate() * 0.25);
        if (fBlinkAcc >= blinkPeriod) { fBlinkAcc = 0; fBlinkOn = !fBlinkOn; }
        fDrawAcc += frames;
        if (fDrawAcc >= 960) { fDrawAcc %= 960; drawWidgets(); }

        fHmi.update({fEngine.state(sel), fEngine.nextAction(sel), fEngine.canUndo(sel), sel, fEngine.isOn(sel),
                     fEngine.transport(), fEngine.armedTrack() == sel, fEngine.userWaiting(sel)});
    }

private:
    ouro::StopMode stopMode() const
    {
        return ouro::StopMode(std::min(2, std::max(0, int(std::lround(fVal[kParamStopMode])))));
    }
    void stopNow(bool mod = false)                  // mod: MOD's transport stopped
    {
        const float fadeMs = fVal[kParamFadeTime] * 1000.0f;
        if (mod) fEngine.modStopped(stopMode(), fadeMs); else fEngine.stop(stopMode(), fadeMs);
    }

    // MOD's grid at this block's start goes to the engine; a play/stop change with a valid grid is an edge, unless
    // init (the first run, or Sync just turned on). BBT fields are read only when valid (DPF: else unset).
    void followMod(bool init, bool click)
    {
        const TimePosition& pos = getTimePosition();
        const TimePosition::BarBeatTick& b = pos.bbt;
        double spb = 0.0, toNext = 0.0;
        if (b.valid)
        {
            const double spBeat = 60.0 / b.beatsPerMinute * getSampleRate();
            spb = b.beatsPerBar * spBeat;
            toNext = (b.beatsPerBar - (b.beat - 1) - b.tick / b.ticksPerBeat) * spBeat;
            if (toNext < 1.0 || toNext > spb - 1.0) toNext = 0.0;   // within a sample of the line: on it
        }
        fEngine.setGrid(b.valid, pos.playing, spb, toNext);
        if (click && b.valid && pos.playing)    // the next beat line from the same position: the click can't drift
        {
            const double spBeat = 60.0 / b.beatsPerMinute * getSampleRate(), into = b.tick / b.ticksPerBeat * spBeat;
            const bool onLine = into < 1.0 || into > spBeat - 1.0;
            fClickBpb = std::max(1, int(std::lround(b.beatsPerBar)));
            fClickBeat = onLine && into < 1.0 ? int(b.beat) : int(b.beat) % fClickBpb + 1;
            fClickAt = onLine ? 0.0 : spBeat - into;
            fClickSpBeat = spBeat;
        }
        if (b.valid && !init && pos.playing != fModPlaying)
        {
            if (pos.playing) fEngine.modStarted(toNext == 0.0); else stopNow(true);
        }
        fModPlaying = pos.playing;
    }

    // The two ticks, 25 ms: a 1000 Hz beat at -6 dB and a 1500 Hz beat 1, decaying, faded to 0 at the end. Non-RT,
    // after fEngine.init().
    void buildTicks(double sr)
    {
        const size_t n = fEngine.enabled() ? size_t(std::lround(0.025 * sr)) : 0;   // a rate it refused: no ticks
        for (int a = 0; a < 2; ++a)
        {
            fTick[a].assign(n, 0.0f);
            const double hz = a ? 1500.0 : 1000.0, amp = a ? 1.0 : 0.5;
            for (size_t i = 0; i < n; ++i)
            {
                const double t = double(i) / sr;
                fTick[a][i] = float(amp * std::sin(2.0 * M_PI * hz * t) * std::exp(-t / 0.006) * (1.0 - double(i) / double(n)));
            }
        }
        fTickPos = kNoTick;
    }

    // The Click output: a tick at each beat line followMod found for this block (fClickAt), a running one carried on.
    void renderClick(float* out, uint32_t frames)
    {
        if (out == nullptr) return;             // not connected (a host may leave a side output off)
        if (fClickAt < 0.0 && fTickPos == kNoTick) { std::fill(out, out + frames, 0.0f); return; }   // a tick finishes
        double at = fClickAt;
        const size_t len = fTick[0].size();
        for (uint32_t i = 0; i < frames; ++i)
        {
            if (at >= 0.0 && double(i) >= at)
            {
                fTickPos = 0; fTickAccent = fClickBeat == 1;
                fClickBeat = fClickBeat % fClickBpb + 1;
                at += fClickSpBeat;
            }
            out[i] = fTickPos < len ? fClickGain * fTick[fTickAccent][fTickPos++] : 0.0f;
            if (fTickPos == len) fTickPos = kNoTick;
        }
    }

    // Renders each shown slot and draws it when the bitmap changed or the handle is new. No allocation: fixed buffers.
    void drawWidgets()
    {
        if (fWidgetDraw == nullptr) return;
        ouro::WidgetView v = {};
        v.selected = fSel;
        v.transport = fEngine.transport();
        v.waiting = fEngine.userWaiting(fSel);
        for (int t = 0; t < ouro::kTracks; ++t)
        {
            v.state[t] = fEngine.state(t); v.on[t] = fEngine.isOn(t); v.armed[t] = fEngine.armedTrack() == t;
            v.layers[t] = fEngine.layerCount(t);   // the bitmap compare below redraws on a count change
        }
        v.loopLength = fEngine.loopLength(); v.position = fEngine.position();
        v.trackLength = fEngine.trackLength(fSel); v.trackPosition = fEngine.trackPosition(fSel);
        v.recordedSamples = fEngine.recordedSamples(); v.sampleRate = fEngine.sampleRate(); v.blinkOn = fBlinkOn;
        v.part = fEngine.part(); v.queuedPart = fEngine.queuedPart();
        v.songStep = fEngine.songStep(); v.songLength = fEngine.songLength(); v.memoryFull = fEngine.memoryFull();
        ouro::SongView sv = {};
        for (int i = 0; i < ouro::kSteps; ++i) { sv.part[i] = uint8_t(fEngine.stepPart(i)); sv.repeats[i] = uint8_t(fEngine.stepRepeats(i)); }
        sv.playing = fEngine.songStep(); sv.on = fEngine.songOn();
        sv.edited = std::min(ouro::kSteps, std::max(1, int(std::lround(fVal[kParamStep])))) - 1;

        uint8_t bits[kMaxBytes];
        for (auto& s : fSlots)
        {
            s.drawing.store(true);                       // seq_cst: pairs with widgetHidden's null store + drawing load
            const ALABS_HMI_Widget w = s.handle.load();
            if (w != nullptr)
            {
                const uint16_t sw = s.w.load(std::memory_order_relaxed), sh = s.h.load(std::memory_order_relaxed);
                const size_t n = s.song.load(std::memory_order_relaxed) ? ouro::renderSongWidget(sv, sw, sh, bits)
                                                                        : ouro::renderLoopWidget(v, sw, sh, bits);
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
    int fPart = 0;                          // the engine's part at the last run(): a switch re-requests track ports
    bool fFirst = true;                     // first run after activate(): apply every value, then snap gains
    uint64_t fHold = 0;                     // samples undo_redo has been held
    bool fHeld = false, fDone = false, fTapPending = false, fStoppedPress = false;
    bool fTapStopped = false;               // the pending tap was released while stopped: a double-tap resumes
    bool fTapCancels = false;               // the tap's press cancelled a wait: it undoes nothing
    uint64_t fSinceTap = 0;
    int fTapTrack = 0;
    bool fModPlaying = false;               // MOD's transport as of the last run() with Sync on
    static constexpr size_t kNoTick = SIZE_MAX;
    std::vector<float> fTick[2];            // the beat's tick and beat 1's (buildTicks)
    size_t fTickPos = kNoTick;              // the running tick's next sample, kNoTick: none
    bool fTickAccent = false;
    double fClickAt = -1.0, fClickSpBeat = 0.0;   // this block's next beat line (< 0: none) and the beat's samples
    int fClickBeat = 1, fClickBpb = 4;      // the next beat line's beat (1 = accented) and the beats in a bar
    float fClickGain = 1.0f;

    ouro::HmiBridge fHmi;                           // labels and LEDs; inert without the host's HMI
    const ALABS_HMI_WidgetDraw* fWidgetDraw = nullptr;  // null: host can't show widgets

    static constexpr uint16_t kMaxW = 128, kMaxH = 64;
    static constexpr size_t kMaxBytes = (kMaxW / 8) * kMaxH;   // 1024

    // One shown widget; handle is null while the slot is free.
    struct WidgetSlot {
        std::atomic<ALABS_HMI_Widget> handle{nullptr};
        std::atomic<uint16_t> w{0}, h{0};
        std::atomic<bool> song{false};      // the song widget (else the loop widget); written before the handle is published
        std::atomic<bool> drawing{false};   // run() is between loading the handle and finishing draw_bitmap
        ALABS_HMI_Widget drawn = nullptr;   // run() only: the handle last drawn (new handle = redraw all)
        uint8_t last[kMaxBytes] = {};       // run() only: the last bitmap drawn
    } fSlots[4];
    uint32_t fDrawAcc = 0, fBlinkAcc = 0;   // samples since the last widget redraw / blink toggle
    bool fBlinkOn = false;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OuroborosPlugin)
};

END_NAMESPACE_DISTRHO
