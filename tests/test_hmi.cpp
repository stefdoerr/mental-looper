#include "hmi/HmiBridge.hpp"
#include "test_util.hpp"

#include <string>
#include <vector>

struct Call { std::string fn; LV2_HMI_Addressing a; std::string text; int colour, on, off; };
static std::vector<Call> gCalls;

static void led(void*, LV2_HMI_Addressing a, LV2_HMI_LED_Colour c, int on, int off) { gCalls.push_back({"led", a, "", c, on, off}); }
static void label(void*, LV2_HMI_Addressing a, const char* t) { gCalls.push_back({"label", a, t, 0, 0, 0}); }
static void value(void*, LV2_HMI_Addressing a, const char* t) { gCalls.push_back({"value", a, t, 0, 0, 0}); }
static LV2_HMI_WidgetControl gFake = { nullptr, sizeof(LV2_HMI_WidgetControl), led, nullptr, label, value, nullptr, nullptr, nullptr };

static LV2_HMI_Addressing A1 = (LV2_HMI_Addressing)0x1, A2 = (LV2_HMI_Addressing)0x2, A3 = (LV2_HMI_Addressing)0x3;
static const uint32_t kAllCaps = LV2_HMI_AddressingCapability_LED | LV2_HMI_AddressingCapability_Label | LV2_HMI_AddressingCapability_Value;

static ouro::HmiView view(ouro::TrackState s, ouro::NextAction n, bool undo = false, int sel = 0, bool on = true)
{
    return { s, n, undo, sel, on };
}
static std::string lastLabel(LV2_HMI_Addressing a)
{
    for (size_t i = gCalls.size(); i-- > 0;) if (gCalls[i].fn == "label" && gCalls[i].a == a) return gCalls[i].text;
    return "<none>";
}
static std::string lastValue(LV2_HMI_Addressing a)
{
    for (size_t i = gCalls.size(); i-- > 0;) if (gCalls[i].fn == "value" && gCalls[i].a == a) return gCalls[i].text;
    return "<none>";
}
static Call lastLed(LV2_HMI_Addressing a)
{
    for (size_t i = gCalls.size(); i-- > 0;) if (gCalls[i].fn == "led" && gCalls[i].a == a) return gCalls[i];
    return {"<none>", a, "", -1, -1, -1};
}
static size_t count(const char* fn) { size_t n = 0; for (auto& c : gCalls) n += c.fn == fn; return n; }

static void labelsSentOnlyOnChange()
{
    gCalls.clear();
    ouro::HmiBridge b; b.setFeature(&gFake);
    b.addressed(ouro::kHmiRec, A1, kAllCaps);
    b.update(view(ouro::TrackState::Empty, ouro::NextAction::Rec));
    CHECK(lastLabel(A1) == "REC" && lastLed(A1).colour == LV2_HMI_LED_Colour_White && lastLed(A1).on == 0);
    size_t n = gCalls.size(); b.update(view(ouro::TrackState::Empty, ouro::NextAction::Rec));
    CHECK(gCalls.size() == n);
    b.update(view(ouro::TrackState::Recording, ouro::NextAction::Play));
    CHECK(lastLabel(A1) == "PLAY" && lastLed(A1).colour == LV2_HMI_LED_Colour_Red && lastLed(A1).on == LV2_HMI_LED_Blink_Mid && lastLed(A1).off == LV2_HMI_LED_Blink_Mid);
    b.update(view(ouro::TrackState::Playing, ouro::NextAction::Dub));
    CHECK(lastLabel(A1) == "DUB" && lastLed(A1).colour == LV2_HMI_LED_Colour_Green && lastLed(A1).on == 0);
    b.update(view(ouro::TrackState::Overdubbing, ouro::NextAction::Play));
    CHECK(lastLed(A1).colour == LV2_HMI_LED_Colour_Green && lastLed(A1).on == LV2_HMI_LED_Blink_Mid);
}

static void undoLabelShowsAvailability()
{
    gCalls.clear();
    ouro::HmiBridge b; b.setFeature(&gFake);
    b.addressed(ouro::kHmiUndo, A1, kAllCaps);
    b.update(view(ouro::TrackState::Empty, ouro::NextAction::Rec, false));
    CHECK(lastLabel(A1) == "-");
    b.update(view(ouro::TrackState::Playing, ouro::NextAction::Dub, true));
    CHECK(lastLabel(A1) == "UNDO");
    CHECK(count("led") == 0);
}

static void knobLabelsFollowTrack()
{
    gCalls.clear();
    ouro::HmiBridge b; b.setFeature(&gFake);
    b.addressed(ouro::kHmiOnOff, A2, kAllCaps);
    b.addressed(ouro::kHmiVolume, A3, kAllCaps);
    b.update(view(ouro::TrackState::Empty, ouro::NextAction::Rec, false, 1, false));
    CHECK(lastLabel(A2) == "OFF 2" && lastValue(A2) == "OFF" && lastLabel(A3) == "Vol 2");
    CHECK(lastValue(A3) == "<none>");
    b.update(view(ouro::TrackState::Empty, ouro::NextAction::Rec, false, 3, true));
    CHECK(lastLabel(A2) == "ON 4" && lastValue(A2) == "ON" && lastLabel(A3) == "Vol 4");
    gCalls.clear();   // toggling on/off on the same track re-sends the label
    b.update(view(ouro::TrackState::Empty, ouro::NextAction::Rec, false, 3, false));
    CHECK(count("label") == 1 && lastLabel(A2) == "OFF 4" && lastValue(A2) == "OFF");
    b.update(view(ouro::TrackState::Empty, ouro::NextAction::Rec, false, 3, true));
    CHECK(lastLabel(A2) == "ON 4");
}

static void readdressResends()
{
    gCalls.clear();
    ouro::HmiBridge b; b.setFeature(&gFake);
    b.addressed(ouro::kHmiRec, A1, kAllCaps);
    b.update(view(ouro::TrackState::Playing, ouro::NextAction::Dub));
    b.unaddressed(ouro::kHmiRec);
    size_t n = gCalls.size();
    b.update(view(ouro::TrackState::Recording, ouro::NextAction::Play));
    CHECK(gCalls.size() == n);                       // unaddressed: nothing sent
    b.update(view(ouro::TrackState::Playing, ouro::NextAction::Dub));
    b.addressed(ouro::kHmiRec, A2, kAllCaps);        // new handle, same view
    b.update(view(ouro::TrackState::Playing, ouro::NextAction::Dub));
    CHECK(lastLabel(A2) == "DUB" && lastLed(A2).colour == LV2_HMI_LED_Colour_Green);
}

static void capsRespected()
{
    gCalls.clear();
    ouro::HmiBridge b; b.setFeature(&gFake);
    b.addressed(ouro::kHmiRec, A1, LV2_HMI_AddressingCapability_Label);
    b.addressed(ouro::kHmiOnOff, A2, LV2_HMI_AddressingCapability_LED);
    b.update(view(ouro::TrackState::Recording, ouro::NextAction::Play));
    CHECK(count("led") == 0 && lastLabel(A1) == "PLAY");
    CHECK(count("label") == 1 && count("value") == 0);   // OnOff has no Label/Value cap
}

static void noFeatureNoCalls()
{
    gCalls.clear();
    ouro::HmiBridge b; b.setFeature(nullptr); b.addressed(ouro::kHmiRec, A1, kAllCaps);
    b.update(view(ouro::TrackState::Recording, ouro::NextAction::Play)); CHECK(gCalls.empty());
}

int main()
{
    labelsSentOnlyOnChange();
    undoLabelShowsAvailability();
    knobLabelsFollowTrack();
    readdressResends();
    capsRespected();
    noFeatureNoCalls();
    REPORT();
}
