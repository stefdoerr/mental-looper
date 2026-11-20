/*
 * Ouroboros — 4-track looper LV2 plugin for the MOD Dwarf.
 * Mono in / mono out. Skeleton: silent output, bypass parameter only.
 */
#pragma once

#include "DistrhoPlugin.hpp"

#include <algorithm>

START_NAMESPACE_DISTRHO

class OuroborosPlugin : public Plugin
{
public:
    enum ParamIndex {
        kParamBypass = 0,
        kNumParams
    };

    OuroborosPlugin() : Plugin(kNumParams, 0, 0) {}

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
    }

private:
    bool fBypass = false;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OuroborosPlugin)
};

END_NAMESPACE_DISTRHO
