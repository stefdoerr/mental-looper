/*
 * Ouroboros — 4-track looper LV2 plugin for the MOD Dwarf.
 * Mono in / mono out. Skeleton: silent output, bypass parameter only.
 */
#pragma once

#include "DistrhoPlugin.hpp"
#include "hmi/alabs-hmi-widgets.h"
#include "hmi/lv2-hmi.h"

#include <algorithm>
#include <atomic>
#include <cstring>

START_NAMESPACE_DISTRHO

// LV2 port index -> parameter index. Valid because the plugin has no event
// ports (no state, MIDI or time position): params follow the audio ports.
constexpr uint32_t paramFromLv2Index(uint32_t lv2Index)
{
    return lv2Index - (DISTRHO_PLUGIN_NUM_INPUTS + DISTRHO_PLUGIN_NUM_OUTPUTS);
}

class OuroborosPlugin : public Plugin
{
public:
    enum ParamIndex {
        kParamBypass = 0,
        kNumParams
    };

    OuroborosPlugin() : Plugin(kNumParams, 0, 0)
    {
        // LV2 only (null for VST3/CLAP): the host's optional HMI features.
        if (const auto* f = static_cast<const LV2_Feature* const*>(d_nextLv2Features))
            for (; *f != nullptr; ++f)
            {
                if (std::strcmp((*f)->URI, LV2_HMI__WidgetControl) == 0)
                    fHmiWc = static_cast<const LV2_HMI_WidgetControl*>((*f)->data);
                else if (std::strcmp((*f)->URI, ALABS_HMI__WidgetDraw) == 0)
                    fWidgetDraw = static_cast<const ALABS_HMI_WidgetDraw*>((*f)->data);
            }
    }

    // LV2 extension callbacks (OuroborosPlugin.cpp), from non-RT host threads,
    // possibly while run() runs: share state with run() through atomics only.
    void hmiAddressed(uint32_t, LV2_HMI_Addressing, const LV2_HMI_AddressingInfo*) {}
    void hmiUnaddressed(uint32_t) {}

    void widgetShown(const char*, ALABS_HMI_Widget w, const ALABS_HMI_WidgetArea* area)
    {
        // As in the aLabs example: take the slot only while free, write its
        // fields, publish the handle last.
        if (fWidgetDraw == nullptr || fSlot.handle.load(std::memory_order_acquire) != nullptr)
            return;
        fSlot.w = area->width;
        fSlot.h = area->height;
        fSlot.fresh = true;
        fSlot.handle.store(w, std::memory_order_release);
    }

    void widgetHidden(ALABS_HMI_Widget w)
    {
        if (fSlot.handle.load(std::memory_order_acquire) == w)
            fSlot.handle.store(nullptr, std::memory_order_release);
    }

protected:
    const char* getLabel()       const override { return DISTRHO_PLUGIN_BRAND; }
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

    void initParameter(uint32_t index, Parameter& parameter) override
    {
        if (index == kParamBypass)
            parameter.initDesignation(kParameterDesignationBypass);
    }

    float getParameterValue(uint32_t index) const override
    {
        return index == kParamBypass && fBypass ? 1.0f : 0.0f;
    }

    void setParameterValue(uint32_t index, float value) override
    {
        if (index == kParamBypass)
            fBypass = value > 0.5f;
    }

    void run(const float**, float** outputs, uint32_t frames) override
    {
        std::fill(outputs[0], outputs[0] + frames, 0.0f);

        // ponytail: placeholder until the widget renderer: one slot, cleared once per showing.
        if (ALABS_HMI_Widget w = fSlot.handle.load(std::memory_order_acquire))
            if (fSlot.fresh)
            {
                fWidgetDraw->draw_bitmap(fWidgetDraw->handle, w, 0, 0, fSlot.w, fSlot.h, nullptr);
                fSlot.fresh = false;
            }
    }

private:
    bool fBypass = false;

    const LV2_HMI_WidgetControl* fHmiWc = nullptr;  // null: host has no MOD HMI
    const ALABS_HMI_WidgetDraw* fWidgetDraw = nullptr;  // null: host can't show widgets

    // One shown widget; handle is null while the slot is free.
    struct WidgetSlot {
        std::atomic<ALABS_HMI_Widget> handle{nullptr};
        uint16_t w = 0, h = 0;
        bool fresh = false;  // not yet drawn since shown
    } fSlot;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OuroborosPlugin)
};

END_NAMESPACE_DISTRHO
