#include "MentalLooperPlugin.hpp"

START_NAMESPACE_DISTRHO

Plugin* createPlugin()
{
    return new MentalLooperPlugin();
}

// LV2 extension data (patches/dpf-lv2-hooks.patch): the MOD HMI and aLabs
// widget notifications, forwarded to the instance. Never called for VST3/CLAP.
static void* (*sInstanceOf)(void* lv2Handle) = nullptr;

static MentalLooperPlugin* self(LV2_Handle h)
{
    return static_cast<MentalLooperPlugin*>(static_cast<Plugin*>(sInstanceOf(h)));
}

const void* d_lv2PluginExtensionData(const char* uri, void* (*instanceOf)(void* lv2Handle))
{
    static const LV2_HMI_PluginNotification hmi = {
        [](LV2_Handle h, uint32_t index, LV2_HMI_Addressing a, const LV2_HMI_AddressingInfo* info) {
            self(h)->hmiAddressed(index, a, info);
        },
        [](LV2_Handle h, uint32_t index) { self(h)->hmiUnaddressed(index); },
    };
    static const ALABS_HMI_WidgetNotification widgets = {
        sizeof(ALABS_HMI_WidgetNotification),
        [](LV2_Handle h, const char* symbol, ALABS_HMI_Widget w, const ALABS_HMI_WidgetArea* area) {
            self(h)->widgetShown(symbol, w, area);
        },
        [](LV2_Handle h, ALABS_HMI_Widget w) { self(h)->widgetHidden(w); },
    };

    sInstanceOf = instanceOf;
    if (std::strcmp(uri, LV2_HMI__PluginNotification) == 0)
        return &hmi;
    if (std::strcmp(uri, ALABS_HMI__WidgetNotification) == 0)
        return &widgets;
    return nullptr;
}

END_NAMESPACE_DISTRHO
