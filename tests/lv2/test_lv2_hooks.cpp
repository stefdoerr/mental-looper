// A fake LV2 host: dlopens the built plugin and drives it through the LV2 C API,
// to check the DPF patch (features in, extension data out). `make test-lv2`.
#include <dlfcn.h>
#include <fstream>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
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
static int portIndex(const std::string& ttlPath, const std::string& symbol);
static float gIn[128], gOut[128], gClick[128], gPorts[128];  // gPorts[i]: port i's lv2:default
static LV2_Atom_Sequence gEvents = { { sizeof(LV2_Atom_Sequence_Body), 0 }, { 0, 0 } };  // lv2_events_in: empty

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
        else if (tok == "lv2:default" && index >= 0 && index < 128)
            ttl >> gPorts[index];
    }
}

static void connectAll(const LV2_Descriptor* d, LV2_Handle h)
{
    d->connect_port(h, 0, gIn);
    d->connect_port(h, 1, gOut);
    d->connect_port(h, 2, gClick);     // the Click output
    d->connect_port(h, 3, &gEvents);   // DPF's events-in port (time position), before the parameters
    for (uint32_t i = 4; i < 128; ++i)
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
    if (!gDraws.empty())   // the first draw covers the whole area, with a real bitmap
        CHECK(gDraws[0].x == 0 && gDraws[0].y == 0 && gDraws[0].w == 40 && gDraws[0].h == 37);
    for (auto& r : gDraws)
        CHECK(r.x + r.w <= 40 && r.y + r.h <= 37 && r.widget == (ALABS_HMI_Widget)0x1 && !r.nullBits);
    wn->widget_shown(h, "other", (ALABS_HMI_Widget)0x9, &area);   // unknown symbol: ignored
    for (int i = 0; i < 20; ++i)
        d->run(h, 128);
    for (auto& r : gDraws)
        CHECK(r.widget != (ALABS_HMI_Widget)0x9);
    wn->widget_hidden(h, (ALABS_HMI_Widget)0x1);
    const size_t n = gDraws.size();
    for (int i = 0; i < 20; ++i)
        d->run(h, 128);
    CHECK(gDraws.size() == n);

    // Hide + re-show racing run(), from another thread while run() is drawing the old handle:
    // widget_hidden must not return before that draw ends, and the new handle gets its whole area drawn.
    ALABS_HMI_WidgetArea small = { sizeof(small), 51, 8, ALABS_HMI_PixelFormat_Mono1 };
    wn->widget_shown(h, "loop", (ALABS_HMI_Widget)0x2, &small);
    std::atomic<bool> hiddenReturned{false};
    std::thread host;
    gDuringDraw = [&] {
        host = std::thread([&] {
            wn->widget_hidden(h, (ALABS_HMI_Widget)0x2);
            hiddenReturned = true;
            wn->widget_shown(h, "loop", (ALABS_HMI_Widget)0x3, &small);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(!hiddenReturned);   // still waiting for this draw to finish
    };
    for (int i = 0; i < 20; ++i)
        d->run(h, 128);
    if (host.joinable()) host.join();
    CHECK(hiddenReturned);
    for (int i = 0; i < 20; ++i)
        d->run(h, 128);
    bool whole = false;
    for (auto& r : gDraws)
        whole |= r.widget == (ALABS_HMI_Widget)0x3 && r.x == 0 && r.y == 0 && r.w == 51 && r.h == 8 && !r.nullBits;
    CHECK(whole);
    d->cleanup(h);
}

// The song widget (128 x 37) is drawn too, whole and with a real bitmap.
static void songWidgetShownLeadsToDraw(const LV2_Descriptor* d)
{
    gDraws.clear();
    LV2_Handle h = d->instantiate(d, 48000, bundleDir.c_str(), fullFeatures);
    connectAll(d, h);
    d->activate(h);
    auto wn = (const ALABS_HMI_WidgetNotification*)d->extension_data(ALABS_HMI__WidgetNotification);
    CHECK(wn != nullptr);
    if (wn == nullptr) { d->cleanup(h); return; }
    ALABS_HMI_WidgetArea area = { sizeof(area), 128, 37, ALABS_HMI_PixelFormat_Mono1 };
    wn->widget_shown(h, "song_steps", (ALABS_HMI_Widget)0x5, &area);
    for (int i = 0; i < 40; ++i)
        d->run(h, 128);
    CHECK(!gDraws.empty());
    if (!gDraws.empty()) CHECK(gDraws[0].x == 0 && gDraws[0].y == 0 && gDraws[0].w == 128 && gDraws[0].h == 37 && !gDraws[0].nullBits);
    wn->widget_hidden(h, (ALABS_HMI_Widget)0x5);
    d->deactivate(h);
    d->cleanup(h);
}

// A host that shows the widget but gave no WidgetDraw feature: nothing to draw with, nothing crashes.
static void widgetShownWithoutDrawFeature(const LV2_Descriptor* d)
{
    static const LV2_Feature* const noDraw[] = { &gOptionsFeature, &gMapFeature, &gHmiFeature, nullptr };
    gDraws.clear();
    LV2_Handle h = d->instantiate(d, 48000, bundleDir.c_str(), noDraw);
    connectAll(d, h);
    d->activate(h);
    auto wn = (const ALABS_HMI_WidgetNotification*)d->extension_data(ALABS_HMI__WidgetNotification);
    ALABS_HMI_WidgetArea area = { sizeof(area), 40, 37, ALABS_HMI_PixelFormat_Mono1 };
    wn->widget_shown(h, "loop", (ALABS_HMI_Widget)0x1, &area);
    for (int i = 0; i < 40; ++i)
        d->run(h, 128);
    wn->widget_hidden(h, (ALABS_HMI_Widget)0x1);
    CHECK(gDraws.empty());
    d->cleanup(h);
}

// Records a 0.5 s loop through the rec port, shows the widget, runs 2 s: the progress bar keeps being redrawn.
static void widgetRedrawsWhenProgressMoves(const LV2_Descriptor* d, const std::string& ttlPath)
{
    gDraws.clear();
    const int rec = portIndex(ttlPath, "rec");
    CHECK(rec == 4);                       // after audio in, loop out, click out and events in
    LV2_Handle h = d->instantiate(d, 48000, bundleDir.c_str(), fullFeatures);
    connectAll(d, h);
    d->activate(h);
    auto wn = (const ALABS_HMI_WidgetNotification*)d->extension_data(ALABS_HMI__WidgetNotification);
    float& recPort = gPorts[rec];
    auto press = [&] { recPort = 1; d->run(h, 128); recPort = 0; d->run(h, 128); };
    press();
    for (int i = 0; i < 187; ++i) d->run(h, 128);   // ~0.5 s
    press();
    ALABS_HMI_WidgetArea area = { sizeof(area), 40, 37, ALABS_HMI_PixelFormat_Mono1 };
    wn->widget_shown(h, "loop", (ALABS_HMI_Widget)0x1, &area);
    for (int i = 0; i < 750; ++i) d->run(h, 128);   // 2 s
    CHECK(gDraws.size() >= 3);
    for (auto& r : gDraws)
        CHECK(!r.nullBits && r.x == 0 && r.y == 0 && r.w == 40 && r.h == 37);
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

// MOD uses rangeSteps as the knob's default step count: 133 gives a 0.5 dB grid through 0 dB on the volume ports.
static void onlyVolumePortsHaveRangeSteps(const std::string& ttlPath)
{
    std::ifstream ttl(ttlPath);
    std::string tok, sym, withSteps;
    while (ttl >> tok)
    {
        if (tok == "lv2:symbol") ttl >> sym;
        else if (tok == "<http://lv2plug.in/ns/ext/port-props#rangeSteps>")
        {
            int n = 0;
            ttl >> n;
            withSteps += sym + "=" + std::to_string(n) + " ";
        }
    }
    CHECK(withSteps == "\"volume\"=133 \"volume_1\"=133 \"volume_2\"=133 \"volume_3\"=133 \"volume_4\"=133 ");
}

static void addressedRecGetsLabel(const LV2_Descriptor* d, const std::string& ttlPath)
{
    gHmiCalls.clear();
    const int idx = portIndex(ttlPath, "rec");
    CHECK(idx == 4);
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
    songWidgetShownLeadsToDraw(d);
    widgetShownWithoutDrawFeature(d);
    addressedRecGetsLabel(d, so.substr(0, so.size() - 3) + ".ttl");
    widgetRedrawsWhenProgressMoves(d, so.substr(0, so.size() - 3) + ".ttl");
    onlyVolumePortsHaveRangeSteps(so.substr(0, so.size() - 3) + ".ttl");
    REPORT();
}
