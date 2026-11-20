// A fake LV2 host: dlopens the built plugin and drives it through the LV2 C API,
// to check the DPF patch (features in, extension data out). `make test-lv2`.
#include <dlfcn.h>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "lv2/atom.h"
#include "lv2/buf-size.h"
#include "lv2/options.h"
#include "lv2/urid.h"
#include "alabs-hmi-widgets.h"
#include "lv2-hmi.h"
#include "../test_util.hpp"

static std::vector<std::string> gUris;

static LV2_URID mapUri(LV2_URID_Map_Handle, const char* uri)
{
    for (size_t i = 0; i < gUris.size(); ++i)
        if (gUris[i] == uri)
            return static_cast<LV2_URID>(i + 1);
    gUris.push_back(uri);
    return static_cast<LV2_URID>(gUris.size());
}

struct Draw { ALABS_HMI_Widget widget; uint16_t x, y, w, h; bool nullBits; };
static std::vector<Draw> gDraws;
static std::function<void()> gDuringDraw;  // once, inside draw_bitmap: a host call racing run()

static void drawBitmap(ALABS_HMI_WidgetDraw_Handle, ALABS_HMI_Widget widget,
                       uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* bits)
{
    gDraws.push_back({ widget, x, y, w, h, bits == nullptr });
    if (gDuringDraw) { auto f = gDuringDraw; gDuringDraw = nullptr; f(); }
}

static int32_t gBlockLength = 128;
static LV2_Options_Option gOptions[2];  // filled in main(); the zeroed last one ends the list
static LV2_URID_Map gMap = { nullptr, mapUri };
struct HmiCall { std::string fn; LV2_HMI_Addressing a; std::string text; };
static std::vector<HmiCall> gHmiCalls;
static void hmiLed(void*, LV2_HMI_Addressing a, LV2_HMI_LED_Colour, int, int) { gHmiCalls.push_back({"led", a, ""}); }
static void hmiLabel(void*, LV2_HMI_Addressing a, const char* t) { gHmiCalls.push_back({"label", a, t}); }
static void hmiValue(void*, LV2_HMI_Addressing a, const char* t) { gHmiCalls.push_back({"value", a, t}); }
static LV2_HMI_WidgetControl gHmi = { nullptr, LV2_HMI_WIDGETCONTROL_SIZE_POPUP_MESSAGE,
                                      hmiLed, nullptr, hmiLabel, hmiValue, nullptr, nullptr, nullptr };
static ALABS_HMI_WidgetDraw gDraw = { sizeof(ALABS_HMI_WidgetDraw), nullptr, drawBitmap };

static LV2_Feature gOptionsFeature = { LV2_OPTIONS__options, gOptions };
static LV2_Feature gMapFeature     = { LV2_URID__map, &gMap };
static LV2_Feature gHmiFeature     = { LV2_HMI__WidgetControl, &gHmi };
static LV2_Feature gDrawFeature    = { ALABS_HMI__WidgetDraw, &gDraw };
static const LV2_Feature* const baseFeatures[] = { &gOptionsFeature, &gMapFeature, nullptr };
static const LV2_Feature* const fullFeatures[] = { &gOptionsFeature, &gMapFeature,
                                                   &gHmiFeature, &gDrawFeature, nullptr };

static std::string bundleDir;
static float gIn[128], gOut[128], gPorts[64];  // gPorts[i]: port i's lv2:default

// Each control port's lv2:default, from the generated TTL next to the .so.
static void readDefaults(const std::string& ttlPath)
{
    std::ifstream ttl(ttlPath);
    CHECK(ttl.is_open());
    std::string tok;
    int index = -1;
    while (ttl >> tok)
    {
        if (tok == "lv2:index")
            ttl >> index;
        else if (tok == "lv2:default" && index >= 0 && index < 64)
            ttl >> gPorts[index];
    }
}

static void connectAll(const LV2_Descriptor* d, LV2_Handle h)
{
    d->connect_port(h, 0, gIn);
    d->connect_port(h, 1, gOut);
    for (uint32_t i = 2; i < 64; ++i)
        d->connect_port(h, i, &gPorts[i]);
}

static void extensionDataForwarded(const LV2_Descriptor* d)
{
    CHECK(d->extension_data(LV2_HMI__PluginNotification) != nullptr);
    CHECK(d->extension_data(ALABS_HMI__WidgetNotification) != nullptr);
    CHECK(d->extension_data("urn:ouroboros:nothing") == nullptr);
}

static void instantiateWithoutFeatures(const LV2_Descriptor* d)
{
    LV2_Handle h = d->instantiate(d, 48000, bundleDir.c_str(), baseFeatures);  // options + urid only
    CHECK(h != nullptr);
    connectAll(d, h);
    d->activate(h);
    for (int i = 0; i < 100; ++i)
        d->run(h, 128);  // no crash, no HMI calls possible
    d->cleanup(h);
}

static void widgetShownLeadsToDraw(const LV2_Descriptor* d)
{
    gDraws.clear();
    LV2_Handle h = d->instantiate(d, 48000, bundleDir.c_str(), fullFeatures);
    connectAll(d, h);
    d->activate(h);
    auto wn = (const ALABS_HMI_WidgetNotification*)d->extension_data(ALABS_HMI__WidgetNotification);
    CHECK(wn != nullptr);
    if (wn == nullptr) { d->cleanup(h); return; }
    ALABS_HMI_WidgetArea area = { sizeof(area), 40, 37, ALABS_HMI_PixelFormat_Mono1 };
    wn->widget_shown(h, "loop", (ALABS_HMI_Widget)0x1, &area);
    for (int i = 0; i < 20; ++i)
        d->run(h, 128);
    CHECK(!gDraws.empty());
    for (auto& r : gDraws)
        CHECK(r.x + r.w <= 40 && r.y + r.h <= 37 && r.widget == (ALABS_HMI_Widget)0x1);
    wn->widget_hidden(h, (ALABS_HMI_Widget)0x1);
    const size_t n = gDraws.size();
    for (int i = 0; i < 20; ++i)
        d->run(h, 128);
    CHECK(gDraws.size() == n);

    // Hide + re-show racing run(): they land while run() is drawing the old
    // handle. The new handle must still get its whole area drawn.
    ALABS_HMI_WidgetArea small = { sizeof(small), 51, 8, ALABS_HMI_PixelFormat_Mono1 };
    wn->widget_shown(h, "loop", (ALABS_HMI_Widget)0x2, &small);
    gDuringDraw = [&] {
        wn->widget_hidden(h, (ALABS_HMI_Widget)0x2);
        wn->widget_shown(h, "loop", (ALABS_HMI_Widget)0x3, &small);
    };
    for (int i = 0; i < 20; ++i)
        d->run(h, 128);
    bool whole = false;
    for (auto& r : gDraws)
        whole |= r.widget == (ALABS_HMI_Widget)0x3 && r.x == 0 && r.y == 0 && r.w == 51 && r.h == 8;
    CHECK(whole);
    d->cleanup(h);
}

// The LV2 index of the port with this symbol, from the generated TTL.
static int portIndex(const std::string& ttlPath, const std::string& symbol)
{
    std::ifstream ttl(ttlPath);
    std::string tok;
    int index = -1;
    while (ttl >> tok)
    {
        if (tok == "lv2:index") ttl >> index;
        else if (tok == "lv2:symbol") { ttl >> tok; if (tok == "\"" + symbol + "\"") return index; }
    }
    return -1;
}

static void addressedRecGetsLabel(const LV2_Descriptor* d, const std::string& ttlPath)
{
    gHmiCalls.clear();
    const int idx = portIndex(ttlPath, "rec");
    CHECK(idx == 2);
    LV2_Handle h = d->instantiate(d, 48000, bundleDir.c_str(), fullFeatures);
    connectAll(d, h);
    d->activate(h);
    auto hn = (const LV2_HMI_PluginNotification*)d->extension_data(LV2_HMI__PluginNotification);
    LV2_HMI_AddressingInfo info = { LV2_HMI_AddressingCapabilities(0x7), LV2_HMI_AddressingFlags(0), "Rec", 0, 1, 0 };
    LV2_HMI_Addressing a = (LV2_HMI_Addressing)0xA1;
    hn->addressed(h, idx, a, &info);
    d->run(h, 128);
    bool label = false;
    for (auto& c : gHmiCalls) label |= c.fn == "label" && c.a == a && c.text == "REC";
    CHECK(label);
    const size_t n = gHmiCalls.size();
    d->run(h, 128);
    CHECK(gHmiCalls.size() == n);   // nothing changed: nothing sent
    d->cleanup(h);
}

int main(int argc, char** argv)
{
    if (argc != 2) { std::printf("usage: %s <bundle>/<plugin>.so\n", argv[0]); return 2; }
    const std::string so = argv[1];
    bundleDir = so.substr(0, so.rfind('/') + 1);
    readDefaults(so.substr(0, so.size() - 3) + ".ttl");

    void* lib = dlopen(so.c_str(), RTLD_NOW);
    if (lib == nullptr) { std::printf("dlopen: %s\n", dlerror()); return 1; }
    auto descriptor = (LV2_Descriptor_Function)dlsym(lib, "lv2_descriptor");
    const LV2_Descriptor* d = descriptor != nullptr ? descriptor(0) : nullptr;
    CHECK(d != nullptr);
    if (d == nullptr) REPORT();

    gOptions[0] = { LV2_OPTIONS_INSTANCE, 0, mapUri(nullptr, LV2_BUF_SIZE__nominalBlockLength),
                    sizeof(int32_t), mapUri(nullptr, LV2_ATOM__Int), &gBlockLength };

    extensionDataForwarded(d);
    instantiateWithoutFeatures(d);
    widgetShownLeadsToDraw(d);
    addressedRecGetsLabel(d, so.substr(0, so.size() - 3) + ".ttl");
    REPORT();
}
