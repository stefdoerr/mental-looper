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
    int lit = 0; for (int x = 1; x < 39; ++x) lit += px(b, 40, x, 30);    // a row inside the bar
    CHECK(std::abs(lit - 19) <= 1);
    CHECK(px(b, 40, 0, 30) == 1 && px(b, 40, 39, 30) == 1);               // outline
    CHECK(litIn(b, 40, 0, 27, 40, 34) > 0 && litIn(b, 40, 0, 23, 40, 27) == 0);   // nothing between marks and bar
}

static void emptyBarWhenNoLoop()
{
    auto v = view(); uint8_t b[1024]; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(litIn(b, 40, 1, 28, 39, 33) == 0);       // no loop, nothing recording: nothing inside the outline
    CHECK(litIn(b, 40, 0, 27, 40, 28) == 40);      // the outline is there
}

static void elapsedShownWhileRecording()
{
    auto v = view(); v.state[0] = TrackState::Recording; v.recordedSamples = 7 * 48000;
    uint8_t b[1024], ref[1024]; ouro::renderLoopWidget(v, 40, 37, b);
    std::memset(ref, 0, sizeof ref); ouro::drawText(ref, 40, 1, 27, "0:07");
    bool same = true;
    for (int y = 27; y < 34; ++y) for (int x = 0; x < 40; ++x) same &= px(b, 40, x, y) == px(ref, 40, x, y);
    CHECK(same);
    CHECK(litIn(ref, 40, 0, 27, 40, 34) > 0);
    v.recordedSamples = 8 * 48000;
    uint8_t b8[1024]; ouro::renderLoopWidget(v, 40, 37, b8);
    CHECK(std::memcmp(b, b8, 5 * 37) != 0);
}

static bool cellIs(const uint8_t* b, int x0, const uint8_t* g)   // 7x7 mark cell at (x0, 15) equals glyph g
{
    for (int r = 0; r < 7; ++r) for (int c = 0; c < 7; ++c) if (px(b, 40, x0 + c, 15 + r) != ((g[r] >> (6 - c)) & 1)) return false;
    return true;
}

static void marksReflectTracks()
{
    auto v = view();
    v.state[0] = TrackState::Playing; v.state[1] = TrackState::Playing; v.state[2] = TrackState::Empty; v.state[3] = TrackState::Recording;
    v.on[1] = false; v.blinkOn = false;
    uint8_t b[1024]; ouro::renderLoopWidget(v, 40, 37, b);
    const int x0[4] = { 1, 11, 21, 31 };    // 7 px marks centred in w/4 = 10 px cells
    CHECK(cellIs(b, x0[0], ouro::kMarkPlay));                                 // playing, on: play triangle
    CHECK(litIn(b, 40, x0[0] + 2, 15, x0[0] + 3, 22) == 5 && litIn(b, 40, x0[0] + 4, 15, x0[0] + 5, 22) == 1);   // asymmetric: left column 5 px, right (apex) 1 px
    CHECK(litIn(b, 40, x0[0] + 3, 15, x0[0] + 4, 22) > litIn(b, 40, x0[0] + 5, 15, x0[0] + 6, 22));
    CHECK(cellIs(b, x0[1], ouro::kMarkStop) && litIn(b, 40, x0[1], 15, x0[1] + 7, 22) == 25);   // playing, muted: 5x5 stop square
    CHECK(litIn(b, 40, x0[2], 15, x0[2] + 7, 22) == 9 && litIn(b, 40, x0[2] + 2, 17, x0[2] + 5, 20) == 9);   // empty: centre dot
    CHECK(litIn(b, 40, x0[3], 15, x0[3] + 7, 22) == 0);                       // recording, blink off
    v.blinkOn = true; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(cellIs(b, x0[3], ouro::kMarkDisc));                                 // recording, blink on: disc
    CHECK(litIn(b, 40, x0[3], 15, x0[3] + 7, 22) == 21);   // 5x5 disc
    for (bool on : { true, false })   // overdubbing: disc regardless of on/off, blinks like recording
    {
        v.state[3] = TrackState::Overdubbing; v.on[3] = on;
        v.blinkOn = false; ouro::renderLoopWidget(v, 40, 37, b);
        CHECK(litIn(b, 40, x0[3], 15, x0[3] + 7, 22) == 0);
        v.blinkOn = true; ouro::renderLoopWidget(v, 40, 37, b);
        CHECK(cellIs(b, x0[3], ouro::kMarkDisc));
    }
    v.state[3] = TrackState::Recording; v.on[3] = false; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(cellIs(b, x0[3], ouro::kMarkDisc));
}

static void selectedFrame()
{
    const int x0[4] = { 1, 11, 21, 31 };
    for (int sel : {0, 2, 3})
        for (bool blink : {false, true})
        {
            auto v = view(); v.selected = sel; v.blinkOn = blink; v.loopLength = 1000;
            v.state[sel] = TrackState::Recording; v.state[1] = TrackState::Playing;   // selected glyph may be blinked off
            uint8_t b[1024]; ouro::renderLoopWidget(v, 40, 27, b);
            for (int i = 0; i < 4; ++i)
            {
                const int fx = x0[i] - 1;   // frame columns fx..fx+8, rows 9..17
                bool frame = true, noFrame = true;
                for (int y = 9; y <= 17; ++y) for (int x = fx; x <= fx + 8; ++x)
                    if (x == fx || x == fx + 8 || y == 9 || y == 17) { frame &= px(b, 40, x, y) == 1; noFrame &= px(b, 40, x, y) == 0; }
                CHECK(i == sel ? frame : noFrame);
                CHECK(litIn(b, 40, fx, 9, fx + 9, 18) - litIn(b, 40, fx + 1, 10, fx + 8, 17) == (i == sel ? 32 : 0));
            }
            CHECK(litIn(b, 40, 0, 0, 40, 1) == 0 && litIn(b, 40, 0, 26, 40, 27) == 0);
            CHECK(litIn(b, 40, 0, 17, 40, 18) == 9 && litIn(b, 40, 0, 19, 40, 20) == 40);   // frame row 17, gap row 18, bar outline row 19: separate rows
        }
}

static void ringsAroundSymbols()   // for every state, selected: 7x7 cell border blank, 9x9 frame border lit
{
    const int x0[4] = { 1, 11, 21, 31 };
    const struct { TrackState s; bool on; } c[] = { {TrackState::Empty, true}, {TrackState::Recording, true}, {TrackState::Recording, false},
        {TrackState::Playing, true}, {TrackState::Playing, false}, {TrackState::Overdubbing, true}, {TrackState::Overdubbing, false} };
    for (auto& e : c) for (int sel = 0; sel < 4; ++sel) for (bool blink : {false, true})
    {
        auto v = view(); v.selected = sel; v.state[sel] = e.s; v.on[sel] = e.on; v.blinkOn = blink;
        uint8_t b[1024]; ouro::renderLoopWidget(v, 40, 37, b);
        const int x = x0[sel];
        CHECK(litIn(b, 40, x - 1, 14, x + 8, 15) == 9 && litIn(b, 40, x - 1, 22, x + 8, 23) == 9);        // frame top/bottom
        CHECK(litIn(b, 40, x - 1, 15, x, 22) == 7 && litIn(b, 40, x + 7, 15, x + 8, 22) == 7);            // frame left/right
        CHECK(litIn(b, 40, x, 15, x + 7, 16) == 0 && litIn(b, 40, x, 21, x + 7, 22) == 0);                // gap top/bottom
        CHECK(litIn(b, 40, x, 16, x + 1, 21) == 0 && litIn(b, 40, x + 6, 16, x + 7, 21) == 0);            // gap left/right
    }
}

static void stateText()
{
    auto v = view(); uint8_t a[1024], b[1024];
    ouro::renderLoopWidget(v, 40, 37, a);
    CHECK(litIn(a, 40, 0, 3, 40, 10) > 0 && litIn(a, 40, 0, 0, 40, 3) == 0 && litIn(a, 40, 0, 10, 40, 14) == 0);
    v.state[0] = TrackState::Overdubbing; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(std::memcmp(a, b, 5 * 13) != 0);
    v.selected = 2; ouro::renderLoopWidget(v, 40, 37, a);
    CHECK(std::memcmp(a, b, 5 * 13) != 0);       // T3 differs from T1
}

static void stateTextNotClipped()
{
    const struct { TrackState s; const char* txt; } c[] = { {TrackState::Empty, "--"}, {TrackState::Recording, "REC"},
        {TrackState::Playing, "PLAY"}, {TrackState::Overdubbing, "DUB"} };
    for (auto& e : c)
    {
        auto v = view(); v.state[0] = e.s; uint8_t b[1024], ref[1024];
        ouro::renderLoopWidget(v, 40, 37, b);
        std::memset(ref, 0, sizeof ref); ouro::drawText(ref, 64, 0, 3, "T1"); ouro::drawText(ref, 64, 14, 3, e.txt);
        bool same = true;
        for (int y = 0; y < 13; ++y) for (int x = 0; x < 40; ++x) same &= px(b, 40, x, y) == px(ref, 64, x, y);
        CHECK(same);
    }
}

static void tallThreshold()
{
    auto v = view(); v.state[2] = TrackState::Empty; uint8_t b[1024];
    for (int h : {20, 26})   // bar only at rows 1..h-2: no text, no marks
    {
        ouro::renderLoopWidget(v, 40, h, b);
        CHECK(litIn(b, 40, 0, 0, 40, h) == 40 * 2 + 2 * (h - 4));   // just the outline
        CHECK(litIn(b, 40, 0, 1, 40, 2) == 40 && litIn(b, 40, 0, h - 2, 40, h - 1) == 40);
    }
    ouro::renderLoopWidget(v, 40, 27, b);   // tall: text, marks and frame above, bar rows 19..25
    CHECK(litIn(b, 40, 0, 1, 40, 8) > 0 && litIn(b, 40, 0, 10, 40, 17) > 0);
    CHECK(litIn(b, 40, 0, 17, 40, 18) == 9 && litIn(b, 40, 0, 18, 40, 19) == 0);   // frame bottom edge only (selected 0)
    CHECK(litIn(b, 40, 0, 19, 40, 20) == 40 && litIn(b, 40, 1, 20, 39, 25) == 0 && litIn(b, 40, 0, 25, 40, 26) == 40);
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

static void layoutRows()
{
    // rows [y0,y1) lit anywhere in the full-width band
    const struct { int h, text, frame, bar; } c[] = { {37, 3, 14, 27}, {27, 1, 9, 19} };
    for (auto e : c)
    {
        auto v = view(); v.loopLength = 1000; v.position = 500; v.state[0] = TrackState::Playing; uint8_t b[1024];
        ouro::renderLoopWidget(v, 40, e.h, b);
        CHECK(litIn(b, 40, 0, e.text, 40, e.text + 7) > 0 && litIn(b, 40, 0, 0, 40, e.text) == 0);
        CHECK(litIn(b, 40, 0, e.text + 7, 40, e.frame) == 0);                         // gap text -> frame
        CHECK(litIn(b, 40, 0, e.frame, 40, e.frame + 1) == 9 && litIn(b, 40, 0, e.frame + 8, 40, e.frame + 9) > 0);   // frame rows
        CHECK(litIn(b, 40, 0, e.frame + 9, 40, e.bar) == 0);                          // gap frame -> bar
        CHECK(litIn(b, 40, 0, e.bar, 40, e.bar + 1) == 40 && litIn(b, 40, 0, e.bar + 6, 40, e.bar + 7) == 40);   // bar rows
        CHECK(litIn(b, 40, 0, e.bar + 7, 40, e.h) == 0);
    }
    for (int h = 27; h <= 64; ++h)   // every height: blank edges, equal-ish gaps, nothing overlapping
    {
        auto v = view(); v.loopLength = 1000; v.position = 500; v.state[0] = TrackState::Playing; uint8_t b[5 * 64];
        ouro::renderLoopWidget(v, 40, h, b);
        const int free = h - 23, base = free / 4, rem = free % 4;
        const int g1 = base + (rem > 0), g2 = base + (rem > 1), text = base, frame = text + 7 + g1, bar = frame + 9 + g2;
        CHECK(litIn(b, 40, 0, 0, 40, 1) == 0 && litIn(b, 40, 0, h - 1, 40, h) == 0 && text >= 1 && h - (bar + 7) >= 1);
        CHECK(litIn(b, 40, 0, text, 40, text + 7) > 0 && litIn(b, 40, 0, text + 7, 40, frame) == 0);
        CHECK(litIn(b, 40, 0, frame, 40, frame + 1) == 9 && litIn(b, 40, 0, frame + 9, 40, bar) == 0);
        CHECK(litIn(b, 40, 0, bar, 40, bar + 1) == 40 && litIn(b, 40, 0, bar + 7, 40, h) == 0);
    }
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
    marksReflectTracks(); selectedFrame(); ringsAroundSymbols(); stateText(); stateTextNotClipped(); tallThreshold(); layoutRows(); shortAreaBarOnly(); topAndBottomRowsBlank();
    REPORT();
}
