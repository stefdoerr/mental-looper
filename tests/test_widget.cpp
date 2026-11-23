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
    int lit = 0; for (int x = 1; x < 39; ++x) lit += px(b, 40, x, 32);    // a row inside the bar
    CHECK(std::abs(lit - 19) <= 1);
    CHECK(px(b, 40, 0, 33) == 1 && px(b, 40, 39, 33) == 1);               // outline
    CHECK(litIn(b, 40, 0, 29, 40, 36) > 0 && litIn(b, 40, 0, 18, 40, 29) == 0);   // nothing between marks and bar
}

static void emptyBarWhenNoLoop()
{
    auto v = view(); uint8_t b[1024]; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(litIn(b, 40, 1, 30, 39, 35) == 0);       // no loop, nothing recording: nothing inside the outline
    CHECK(litIn(b, 40, 0, 29, 40, 30) == 40);      // the outline is there
}

static void elapsedShownWhileRecording()
{
    auto v = view(); v.state[0] = TrackState::Recording; v.recordedSamples = 7 * 48000;
    uint8_t b[1024], ref[1024]; ouro::renderLoopWidget(v, 40, 37, b);
    std::memset(ref, 0, sizeof ref); ouro::drawText(ref, 40, 1, 29, "0:07");
    bool same = true;
    for (int y = 29; y < 36; ++y) for (int x = 0; x < 40; ++x) same &= px(b, 40, x, y) == px(ref, 40, x, y);
    CHECK(same);
    CHECK(litIn(ref, 40, 0, 29, 40, 36) > 0);
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
    CHECK(litIn(b, 40, x0[0], 11, x0[0] + 7, 18) == 49);
    CHECK(litIn(b, 40, x0[1], 11, x0[1] + 7, 18) == 24 && litIn(b, 40, x0[1] + 1, 12, x0[1] + 6, 17) == 0);
    CHECK(litIn(b, 40, x0[2], 11, x0[2] + 7, 18) == 9 && litIn(b, 40, x0[2] + 2, 13, x0[2] + 5, 16) == 9);
    CHECK(litIn(b, 40, x0[3], 11, x0[3] + 7, 18) == 0);
    v.blinkOn = true; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(litIn(b, 40, x0[3], 11, x0[3] + 7, 18) == 49);
}

static void stateText()
{
    auto v = view(); uint8_t a[1024], b[1024];
    ouro::renderLoopWidget(v, 40, 37, a);
    CHECK(litIn(a, 40, 0, 1, 40, 8) > 0 && litIn(a, 40, 0, 8, 40, 11) == 0);
    v.state[0] = TrackState::Overdubbing; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(std::memcmp(a, b, 5 * 9) != 0);
    v.selected = 2; ouro::renderLoopWidget(v, 40, 37, a);
    CHECK(std::memcmp(a, b, 5 * 9) != 0);       // T3 differs from T1
}

static void stateTextNotClipped()
{
    const struct { TrackState s; const char* txt; } c[] = { {TrackState::Empty, "--"}, {TrackState::Recording, "REC"},
        {TrackState::Playing, "PLAY"}, {TrackState::Overdubbing, "DUB"} };
    for (auto& e : c)
    {
        auto v = view(); v.state[0] = e.s; uint8_t b[1024], ref[1024];
        ouro::renderLoopWidget(v, 40, 37, b);
        std::memset(ref, 0, sizeof ref); ouro::drawText(ref, 64, 0, 1, "T1"); ouro::drawText(ref, 64, 14, 1, e.txt);
        bool same = true;
        for (int y = 0; y < 9; ++y) for (int x = 0; x < 40; ++x) same &= px(b, 40, x, y) == px(ref, 64, x, y);
        CHECK(same);
    }
}

static void tallThreshold()
{
    auto v = view(); v.state[2] = TrackState::Empty; uint8_t b[1024];
    for (int h : {20, 25})   // bar only at rows 1..h-2: no text, no marks
    {
        ouro::renderLoopWidget(v, 40, h, b);
        CHECK(litIn(b, 40, 0, 0, 40, h) == 40 * 2 + 2 * (h - 4));   // just the outline
        CHECK(litIn(b, 40, 0, 1, 40, 2) == 40 && litIn(b, 40, 0, h - 2, 40, h - 1) == 40);
    }
    ouro::renderLoopWidget(v, 40, 26, b);   // tall: text and marks above, bar rows 18..24
    CHECK(litIn(b, 40, 0, 1, 40, 8) > 0 && litIn(b, 40, 0, 11, 40, 18) > 0);
    CHECK(litIn(b, 40, 0, 18, 40, 19) == 40 && litIn(b, 40, 1, 19, 39, 24) == 0 && litIn(b, 40, 0, 24, 40, 25) == 40);
}

static void shortAreaBarOnly()
{
    auto v = view(); v.loopLength = 100; v.position = 100; uint8_t b[1024];
    CHECK(ouro::renderLoopWidget(v, 51, 8, b) == 7 * 8);
    CHECK(litIn(b, 51, 0, 1, 51, 2) == 51 && litIn(b, 51, 0, 6, 51, 7) == 51);   // rows 1..6 are the bar
    CHECK(litIn(b, 51, 1, 2, 50, 6) == 49 * 4);                                  // full: filled; no text
    v.position = 0; v.state[0] = TrackState::Playing; ouro::renderLoopWidget(v, 51, 8, b);
    CHECK(litIn(b, 51, 1, 2, 50, 6) == 0);
}

static void topAndBottomRowsBlank()
{
    auto v = view(); v.loopLength = 1000; v.position = 500;
    for (int t = 0; t < ouro::kTracks; ++t) v.state[t] = TrackState::Playing;
    v.state[3] = TrackState::Recording; v.blinkOn = true;
    const struct { int w, h; } sz[] = { {40, 37}, {51, 8}, {40, 25}, {40, 26}, {128, 64} };
    for (auto s : sz)
    {
        uint8_t b[128 / 8 * 64];
        ouro::renderLoopWidget(v, s.w, s.h, b);
        CHECK(litIn(b, s.w, 0, 0, s.w, 1) == 0 && litIn(b, s.w, 0, s.h - 1, s.w, s.h) == 0);
        CHECK(litIn(b, s.w, 0, 1, s.w, s.h - 1) > 0);
        auto r = view(); r.state[0] = TrackState::Recording; r.recordedSamples = 7 * 48000;   // elapsed text
        ouro::renderLoopWidget(r, s.w, s.h, b);
        CHECK(litIn(b, s.w, 0, 0, s.w, 1) == 0 && litIn(b, s.w, 0, s.h - 1, s.w, s.h) == 0);
    }
}

int main()
{
    sizeIsMono1(); barFillsProportionally(); emptyBarWhenNoLoop(); elapsedShownWhileRecording();
    marksReflectTracks(); stateText(); stateTextNotClipped(); tallThreshold(); shortAreaBarOnly(); topAndBottomRowsBlank();
    REPORT();
}
