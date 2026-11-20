#include "hmi/LoopWidget.hpp"
#include "test_util.hpp"

#include <cstdlib>
#include <cstring>

using ouro::TrackState;

static int px(const uint8_t* b, int w, int x, int y) { return (b[y * ((w + 7) / 8) + x / 8] >> (7 - x % 8)) & 1; }

static ouro::WidgetView view()
{
    ouro::WidgetView v = {};
    v.selected = 0;
    for (int t = 0; t < ouro::kTracks; ++t) { v.state[t] = TrackState::Empty; v.on[t] = true; }
    v.sampleRate = 48000;
    return v;
}

static int litIn(const uint8_t* b, int w, int x0, int y0, int x1, int y1)   // [x0,x1) x [y0,y1)
{
    int n = 0;
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) n += px(b, w, x, y);
    return n;
}

static void sizeIsMono1() { uint8_t b[1024]; CHECK(ouro::renderLoopWidget(view(), 40, 37, b) == 5 * 37); }

static void barFillsProportionally()
{
    auto v = view(); v.loopLength = 1000; v.position = 500; uint8_t b[1024]; ouro::renderLoopWidget(v, 40, 37, b);
    int lit = 0; for (int x = 1; x < 39; ++x) lit += px(b, 40, x, 33);    // a row inside the bar
    CHECK(std::abs(lit - 19) <= 1);
    CHECK(px(b, 40, 0, 33) == 1 && px(b, 40, 39, 33) == 1);               // outline
    CHECK(litIn(b, 40, 0, 30, 40, 37) > 0 && litIn(b, 40, 0, 20, 40, 30) == 0);   // nothing between marks and bar
}

static void emptyBarWhenNoLoop()
{
    auto v = view(); uint8_t b[1024]; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(litIn(b, 40, 1, 31, 39, 36) == 0);       // no loop, nothing recording: nothing inside the outline
    CHECK(litIn(b, 40, 0, 30, 40, 31) == 40);      // the outline is there
}

static void elapsedShownWhileRecording()
{
    auto v = view(); v.state[0] = TrackState::Recording; v.recordedSamples = 7 * 48000;
    uint8_t b[1024], ref[1024]; ouro::renderLoopWidget(v, 40, 37, b);
    std::memset(ref, 0, sizeof ref); ouro::drawText(ref, 40, 1, 30, "0:07");
    bool same = true;
    for (int y = 30; y < 37; ++y) for (int x = 0; x < 40; ++x) same &= px(b, 40, x, y) == px(ref, 40, x, y);
    CHECK(same);
    CHECK(litIn(ref, 40, 0, 30, 40, 37) > 0);
    v.recordedSamples = 8 * 48000;
    uint8_t b8[1024]; ouro::renderLoopWidget(v, 40, 37, b8);
    CHECK(std::memcmp(b, b8, 5 * 37) != 0);
}

static void marksReflectTracks()
{
    auto v = view();
    v.state[0] = TrackState::Playing; v.state[1] = TrackState::Playing; v.state[2] = TrackState::Empty; v.state[3] = TrackState::Recording;
    v.on[1] = false; v.blinkOn = false;
    uint8_t b[1024]; ouro::renderLoopWidget(v, 40, 37, b);
    const int x0[4] = { 1, 11, 21, 31 };    // 7 px marks centred in w/4 = 10 px cells
    CHECK(litIn(b, 40, x0[0], 10, x0[0] + 7, 17) == 49);
    CHECK(litIn(b, 40, x0[1], 10, x0[1] + 7, 17) == 24 && litIn(b, 40, x0[1] + 1, 11, x0[1] + 6, 16) == 0);
    CHECK(litIn(b, 40, x0[2], 10, x0[2] + 7, 17) == 9 && litIn(b, 40, x0[2] + 2, 12, x0[2] + 5, 15) == 9);
    CHECK(litIn(b, 40, x0[3], 10, x0[3] + 7, 17) == 0);
    v.blinkOn = true; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(litIn(b, 40, x0[3], 10, x0[3] + 7, 17) == 49);
}

static void stateText()
{
    auto v = view(); uint8_t a[1024], b[1024];
    ouro::renderLoopWidget(v, 40, 37, a);
    CHECK(litIn(a, 40, 0, 0, 40, 8) > 0 && litIn(a, 40, 0, 8, 40, 10) == 0);
    v.state[0] = TrackState::Overdubbing; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(std::memcmp(a, b, 5 * 8) != 0);
    v.selected = 2; ouro::renderLoopWidget(v, 40, 37, a);
    CHECK(std::memcmp(a, b, 5 * 8) != 0);       // T3 differs from T1
}

static void stateTextNotClipped()
{
    const struct { TrackState s; const char* txt; } c[] = { {TrackState::Empty, "--"}, {TrackState::Recording, "REC"},
        {TrackState::Playing, "PLAY"}, {TrackState::Overdubbing, "DUB"} };
    for (auto& e : c)
    {
        auto v = view(); v.state[0] = e.s; uint8_t b[1024], ref[1024];
        ouro::renderLoopWidget(v, 40, 37, b);
        std::memset(ref, 0, sizeof ref); ouro::drawText(ref, 64, 0, 0, "T1"); ouro::drawText(ref, 64, 14, 0, e.txt);
        bool same = true;
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 40; ++x) same &= px(b, 40, x, y) == px(ref, 64, x, y);
        CHECK(same);
    }
}

static void tallThreshold()
{
    auto v = view(); v.state[2] = TrackState::Empty; uint8_t b[1024];
    for (int h : {20, 23})   // bar only: no text, no marks
    {
        ouro::renderLoopWidget(v, 40, h, b);
        CHECK(litIn(b, 40, 0, 0, 40, h) == 40 * 2 + 2 * (h - 2));   // just the outline
    }
    ouro::renderLoopWidget(v, 40, 24, b);   // tall: text and marks above, bar rows 17..23
    CHECK(litIn(b, 40, 0, 0, 40, 8) > 0 && litIn(b, 40, 0, 10, 40, 17) > 0);
    CHECK(litIn(b, 40, 0, 17, 40, 18) == 40 && litIn(b, 40, 1, 18, 39, 23) == 0);
}

static void shortAreaBarOnly()
{
    auto v = view(); v.loopLength = 100; v.position = 100; uint8_t b[1024];
    CHECK(ouro::renderLoopWidget(v, 51, 8, b) == 7 * 8);
    CHECK(litIn(b, 51, 0, 0, 51, 1) == 51 && litIn(b, 51, 0, 7, 51, 8) == 51);   // rows 0..7 are the bar
    CHECK(litIn(b, 51, 1, 1, 50, 7) == 49 * 6);                                  // full: filled; no text
    v.position = 0; v.state[0] = TrackState::Playing; ouro::renderLoopWidget(v, 51, 8, b);
    CHECK(litIn(b, 51, 1, 1, 50, 7) == 0);
}

int main()
{
    sizeIsMono1(); barFillsProportionally(); emptyBarWhenNoLoop(); elapsedShownWhileRecording();
    marksReflectTracks(); stateText(); stateTextNotClipped(); tallThreshold(); shortAreaBarOnly();
    REPORT();
}
