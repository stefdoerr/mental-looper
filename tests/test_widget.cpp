#include "hmi/LoopWidget.hpp"
#include "hmi/SongWidget.hpp"
#include "hmi/PiecesWidget.hpp"
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
    v.sampleRate = 48000; v.queuedPart = -1; v.songStep = -1;   // 0 would be a queued part A / the song's first step
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
    CHECK(litIn(b, 40, 0, 28, 40, 35) > 0 && litIn(b, 40, 0, 20, 40, 28) == 0);   // nothing between marks and bar (no counts)
}

static void emptyBarWhenNoLoop()
{
    auto v = view(); uint8_t b[1024]; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(litIn(b, 40, 1, 29, 39, 34) == 0);       // no loop, nothing recording: nothing inside the outline
    CHECK(litIn(b, 40, 0, 28, 40, 29) == 40);      // the outline is there
}

static void elapsedShownWhileRecording()
{
    auto v = view(); v.state[0] = TrackState::Recording; v.recordedSamples = 7 * 48000;
    uint8_t b[1024], ref[1024]; ouro::renderLoopWidget(v, 40, 37, b);
    std::memset(ref, 0, sizeof ref); ouro::drawText(ref, 40, 1, 28, "0:07");
    bool same = true;
    for (int y = 28; y < 35; ++y) for (int x = 0; x < 40; ++x) same &= px(b, 40, x, y) == px(ref, 40, x, y);
    CHECK(same);
    CHECK(litIn(ref, 40, 0, 28, 40, 35) > 0);
    v.recordedSamples = 8 * 48000;
    uint8_t b8[1024]; ouro::renderLoopWidget(v, 40, 37, b8);
    CHECK(std::memcmp(b, b8, 5 * 37) != 0);
}

static bool cellIs(const uint8_t* b, int x0, const uint8_t* g)   // 7x7 mark cell at (x0, 12) equals glyph g
{
    for (int r = 0; r < 7; ++r) for (int c = 0; c < 7; ++c) if (px(b, 40, x0 + c, 12 + r) != ((g[r] >> (6 - c)) & 1)) return false;
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
    CHECK(litIn(b, 40, x0[0] + 2, 12, x0[0] + 3, 19) == 5 && litIn(b, 40, x0[0] + 4, 12, x0[0] + 5, 19) == 1);   // asymmetric: left column 5 px, right (apex) 1 px
    CHECK(litIn(b, 40, x0[0] + 3, 12, x0[0] + 4, 19) > litIn(b, 40, x0[0] + 5, 12, x0[0] + 6, 19));
    CHECK(cellIs(b, x0[1], ouro::kMarkStop) && litIn(b, 40, x0[1], 12, x0[1] + 7, 19) == 25);   // playing, muted: 5x5 stop square
    CHECK(litIn(b, 40, x0[2], 12, x0[2] + 7, 19) == 9 && litIn(b, 40, x0[2] + 2, 14, x0[2] + 5, 17) == 9);   // empty: centre dot
    CHECK(litIn(b, 40, x0[3], 12, x0[3] + 7, 19) == 0);                       // recording, blink off
    v.blinkOn = true; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(cellIs(b, x0[3], ouro::kMarkDisc));                                 // recording, blink on: disc
    CHECK(litIn(b, 40, x0[3], 12, x0[3] + 7, 19) == 21);   // 5x5 disc
    for (bool on : { true, false })   // overdubbing: disc regardless of on/off, blinks like recording
    {
        v.state[3] = TrackState::Overdubbing; v.on[3] = on;
        v.blinkOn = false; ouro::renderLoopWidget(v, 40, 37, b);
        CHECK(litIn(b, 40, x0[3], 12, x0[3] + 7, 19) == 0);
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
        CHECK(litIn(b, 40, x - 1, 11, x + 8, 12) == 9 && litIn(b, 40, x - 1, 19, x + 8, 20) == 9);        // frame top/bottom
        CHECK(litIn(b, 40, x - 1, 12, x, 19) == 7 && litIn(b, 40, x + 7, 12, x + 8, 19) == 7);            // frame left/right
        CHECK(litIn(b, 40, x, 12, x + 7, 13) == 0 && litIn(b, 40, x, 18, x + 7, 19) == 0);                // gap top/bottom
        CHECK(litIn(b, 40, x, 13, x + 1, 18) == 0 && litIn(b, 40, x + 6, 13, x + 7, 18) == 0);            // gap left/right
    }
}

static void stateText()
{
    auto v = view(); uint8_t a[1024], b[1024];
    ouro::renderLoopWidget(v, 40, 37, a);
    CHECK(litIn(a, 40, 0, 2, 40, 9) > 0 && litIn(a, 40, 0, 0, 40, 2) == 0 && litIn(a, 40, 0, 9, 40, 11) == 0);
    v.state[0] = TrackState::Overdubbing; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(std::memcmp(a, b, 5 * 11) != 0);
    v.selected = 2; ouro::renderLoopWidget(v, 40, 37, a);
    CHECK(std::memcmp(a, b, 5 * 11) != 0);       // T3 differs from T1
}

static void stateTextNotClipped()
{
    const struct { TrackState s; const char* txt; } c[] = { {TrackState::Empty, "--"}, {TrackState::Recording, "REC"},
        {TrackState::Playing, "PLAY"}, {TrackState::Overdubbing, "DUB"} };
    for (auto& e : c)
    {
        auto v = view(); v.state[0] = e.s; uint8_t b[1024], ref[1024];
        ouro::renderLoopWidget(v, 40, 37, b);
        std::memset(ref, 0, sizeof ref); ouro::drawText(ref, 64, 0, 2, "A1"); ouro::drawText(ref, 64, 14, 2, e.txt);
        bool same = true;
        for (int y = 0; y < 11; ++y) for (int x = 0; x < 40; ++x) same &= px(b, 40, x, y) == px(ref, 64, x, y);
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
    const struct { int h, text, frame, bar; } c[] = { {37, 2, 11, 28}, {27, 1, 9, 19} };
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
    // Every height: blank edges, equal-ish gaps, nothing overlapping. From 33 rows the counts (5 rows) sit 1 row under the
    // frame; below, the layout is the one without them, and a count draws nothing.
    for (int h = 27; h <= 64; ++h)
    {
        auto v = view(); v.loopLength = 1000; v.position = 500; v.state[0] = TrackState::Playing; v.layers[0] = 8; uint8_t b[5 * 64];
        ouro::renderLoopWidget(v, 40, h, b);
        const bool counts = h >= 33;
        const int free = h - (counts ? 29 : 23), base = free / 4, rem = free % 4;
        const int g1 = base + (rem > 0), g2 = base + (rem > 1), text = base, frame = text + 7 + g1, bar = frame + (counts ? 15 : 9) + g2;
        CHECK(litIn(b, 40, 0, 0, 40, 1) == 0 && litIn(b, 40, 0, h - 1, 40, h) == 0 && text >= 1 && h - (bar + 7) >= 1);
        CHECK(litIn(b, 40, 0, text, 40, text + 7) > 0 && litIn(b, 40, 0, text + 7, 40, frame) == 0);
        CHECK(litIn(b, 40, 0, frame, 40, frame + 1) == 9);
        if (counts) CHECK(litIn(b, 40, 0, frame + 9, 40, frame + 10) == 0 && litIn(b, 40, 0, frame + 10, 40, frame + 15) == 13
                          && litIn(b, 40, 0, frame + 15, 40, bar) == 0);   // an 8: 13 px
        else CHECK(litIn(b, 40, 0, frame + 9, 40, bar) == 0);
        CHECK(litIn(b, 40, 0, bar, 40, bar + 1) == 40 && litIn(b, 40, 0, bar + 7, 40, h) == 0);
    }
}

static void topAndBottomRowsBlank()
{
    auto v = view(); v.loopLength = 1000; v.position = 500;
    for (int t = 0; t < ouro::kTracks; ++t) v.state[t] = TrackState::Playing;
    v.state[3] = TrackState::Recording; v.blinkOn = true;
    for (int t = 0; t < ouro::kTracks; ++t) v.layers[t] = 88;
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

// The text band x 14..39, rows 2..8 matches s drawn alone there.
static bool showsText(const ouro::WidgetView& v, const char* s)
{
    uint8_t b[1024], want[1024] = {}; ouro::renderLoopWidget(v, 40, 37, b); ouro::drawText(want, 40, 14, 2, s);
    for (int y = 2; y < 9; ++y) for (int x = 14; x < 40; ++x) if (px(b, 40, x, y) != px(want, 40, x, y)) return false;
    return true;
}
static void stoppedShowsStop()
{
    auto v = view(); v.state[0] = TrackState::Playing; v.loopLength = 1000; v.transport = ouro::Transport::Stopped;
    CHECK(showsText(v, "STOP"));
    v.transport = ouro::Transport::Stopping; v.blinkOn = true;  CHECK(showsText(v, "STOP"));
    v.blinkOn = false; CHECK(showsText(v, ""));
}
static void armedShowsArmAndBlinksItsDot()
{
    auto v = view(); v.armed[0] = true; uint8_t b[1024];
    v.blinkOn = true;  ouro::renderLoopWidget(v, 40, 37, b); CHECK(showsText(v, "ARM") && litIn(b, 40, 3, 14, 6, 17) == 9);
    v.blinkOn = false; ouro::renderLoopWidget(v, 40, 37, b); CHECK(showsText(v, "ARM") && litIn(b, 40, 3, 14, 6, 17) == 0);
}

static void waitShowsWaitInPlaceOfTheState()
{
    auto v = view(); v.state[0] = TrackState::Playing; v.loopLength = 1000; v.layers[0] = 2; v.blinkOn = true;
    uint8_t a[1024], b[1024]; ouro::renderLoopWidget(v, 40, 37, a);
    v.waiting = true; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(showsText(v, "WAIT"));
    v.waiting = false; CHECK(showsText(v, "PLAY"));
    for (int y = 9; y < 37; ++y) for (int x = 0; x < 40; ++x) CHECK(px(a, 40, x, y) == px(b, 40, x, y));   // marks, counts, bar untouched
}

// Every character the widget can draw has a glyph (showsText compares against the same font, so it can't catch a gap).
static void everyWidgetStringHasGlyphs()
{
    static const char* const strings[] = { "--", "REC", "PLAY", "DUB", "STOP", "ARM", "WAIT", "T1", "T2", "T3", "T4", "0123456789:",
                                            "3 FULL", "3 EMPTY", "SAVE 3?", "LOADING 40%", "SAVED 3", "120 BPM 4 BARS", "1 BAR", "DISK FULL",
                                            "SAVE ERROR", "TOO BIG", "LOAD ERROR", "EMPTY", "SAVING", "LOAD", "LOADED 16" };
    for (const char* s : strings)
        for (const char* c = s; *c != '\0'; ++c)
        {
            if (*c == ' ') continue;
            const uint8_t* g = ouro::glyph(*c);
            int lit = 0; for (int r = 0; r < 7; ++r) lit += g[r] != 0;
            if (lit == 0) std::printf("  no glyph for '%c' in \"%s\"\n", *c, s);
            CHECK(lit > 0);
        }
    for (int d = 0; d < 10; ++d)   // the 3x5 count digits: each lit, 3 px wide, and unlike the others
    {
        int lit = 0; for (int r = 0; r < 5; ++r) { lit += ouro::kDigits3x5[d][r] != 0; CHECK(ouro::kDigits3x5[d][r] < 8); }
        CHECK(lit > 0);
        for (int e = 0; e < d; ++e) CHECK(std::memcmp(ouro::kDigits3x5[d], ouro::kDigits3x5[e], 5) != 0);
    }
}

// The layer counts at h = 37: 3x5 digits in rows 21..25, centred under each mark (two digits fill its 7 columns 1 px
// apart, one takes the middle 3); a track at 0 (empty) shows none.
static void countsUnderTheirMarks()
{
    const int x0[4] = { 1, 11, 21, 31 };
    const uint32_t n[4] = { 3, 12, 0, 7 };
    auto v = view(); v.blinkOn = true;
    for (int i = 0; i < 4; ++i) { v.layers[i] = n[i]; v.state[i] = n[i] ? TrackState::Playing : TrackState::Empty; }
    uint8_t b[1024], ref[1024] = {}; ouro::renderLoopWidget(v, 40, 37, b);
    for (int i = 0; i < 4; ++i) if (n[i]) ouro::drawCount(ref, 40, x0[i], 21, n[i]);
    bool same = true;
    for (int y = 20; y < 28; ++y) for (int x = 0; x < 40; ++x) same &= px(b, 40, x, y) == px(ref, 40, x, y);
    CHECK(same);
    const int x = x0[0], y = x0[1];
    CHECK(litIn(b, 40, x, 21, x + 2, 26) == 0 && litIn(b, 40, x + 2, 21, x + 5, 26) == 11 && litIn(b, 40, x + 5, 21, x + 7, 26) == 0);   // "3"
    CHECK(litIn(b, 40, y, 21, y + 3, 26) == 8 && litIn(b, 40, y + 3, 21, y + 4, 26) == 0 && litIn(b, 40, y + 4, 21, y + 7, 26) == 11);  // "12"
    for (int r = 0; r < 5; ++r) for (int c = 0; c < 3; ++c)
        CHECK(px(b, 40, y + c, 21 + r) == ((ouro::kDigits3x5[1][r] >> (2 - c)) & 1) && px(b, 40, y + 4 + c, 21 + r) == ((ouro::kDigits3x5[2][r] >> (2 - c)) & 1));
    CHECK(litIn(b, 40, x0[2] - 1, 20, x0[2] + 8, 28) == 0);                    // track 3: 0, nothing
    CHECK(litIn(b, 40, x0[3] + 2, 21, x0[3] + 5, 26) > 0);
    CHECK(litIn(b, 40, 0, 20, 40, 21) == 0 && litIn(b, 40, 0, 26, 40, 28) == 0);
}
static void countsAbove99Show99()
{
    auto v = view(); v.state[0] = TrackState::Playing; uint8_t a[1024], b[1024];
    v.layers[0] = 150; ouro::renderLoopWidget(v, 40, 37, a);
    v.layers[0] = 99;  ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(std::memcmp(a, b, 5 * 37) == 0);
    for (int r = 0; r < 5; ++r) for (int c = 0; c < 3; ++c)
        CHECK(px(a, 40, 1 + c, 21 + r) == ((ouro::kDigits3x5[9][r] >> (2 - c)) & 1) && px(a, 40, 5 + c, 21 + r) == px(a, 40, 1 + c, 21 + r));
}
// h = 37 with everything drawn: text 2..8, frame 11..19, counts 21..25, bar 28..34, and blank rows between.
static void countRowsDontOverlap()
{
    auto v = view(); v.loopLength = 1000; v.position = 500; v.blinkOn = true;
    for (int t = 0; t < ouro::kTracks; ++t) { v.state[t] = TrackState::Playing; v.layers[t] = 88; }
    uint8_t b[1024]; ouro::renderLoopWidget(v, 40, 37, b);
    const struct { int y0, y1; bool lit; } band[] = { {0, 2, false}, {2, 9, true}, {9, 11, false}, {11, 20, true}, {20, 21, false},
        {21, 26, true}, {26, 28, false}, {28, 35, true}, {35, 37, false} };
    for (auto e : band)
        for (int y = e.y0; y < e.y1; ++y) CHECK((litIn(b, 40, 0, y, 40, y + 1) > 0) == e.lit);
    CHECK(litIn(b, 40, 0, 21, 40, 26) == 4 * 2 * 13);                          // eight 8s
}
// Below 33 rows the counts don't fit: the layout without them, pixel for pixel, whatever the counts.
static void noCountsWhenShort()
{
    for (int h = 20; h <= 33; ++h)
    {
        auto v = view(); v.loopLength = 1000; v.position = 500; v.state[0] = TrackState::Playing; uint8_t a[1024], b[1024];
        ouro::renderLoopWidget(v, 40, h, a);
        v.layers[0] = 5; ouro::renderLoopWidget(v, 40, h, b);
        CHECK((std::memcmp(a, b, 5 * h) == 0) == (h < 33));
    }
}

static void longTrackBarHasPassTicks()
{
    auto v = view(); v.state[0] = TrackState::Playing; v.loopLength = 1000; v.trackLength = 3000; uint8_t b[1024];
    v.trackPosition = 0; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(px(b, 40, 14, 29) == 1 && px(b, 40, 26, 29) == 1 && px(b, 40, 13, 29) == 0 && px(b, 40, 15, 29) == 0);
    v.trackPosition = 1500; ouro::renderLoopWidget(v, 40, 37, b);   // half: filled to column 19
    CHECK(px(b, 40, 14, 29) == 0 && px(b, 40, 13, 29) == 1 && px(b, 40, 26, 29) == 1 && px(b, 40, 25, 29) == 0);
    int lit = 0; for (int x = 1; x < 39; ++x) lit += px(b, 40, x, 30);
    CHECK(std::abs(lit - 19) <= 1);
}
static void crowdedTicksAreLeftOut()                                   // Review Focus 5: 20 loops, 1.9 px apart
{
    auto v = view(); v.state[0] = TrackState::Playing; v.loopLength = 100; v.trackLength = 2000; v.trackPosition = 0;
    uint8_t b[1024]; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(litIn(b, 40, 1, 29, 39, 30) == 0);
}
static void recordingShowsPassesSoFar()
{
    auto v = view(); v.state[0] = TrackState::Recording; v.loopLength = 1000; v.position = 0; v.trackPosition = 2500;
    uint8_t b[1024]; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(px(b, 40, 2, 33) == 1 && px(b, 40, 4, 33) == 1 && px(b, 40, 6, 33) == 0);
    v.position = 500; ouro::renderLoopWidget(v, 40, 37, b);   // filled to column 19: the dots show dark
    CHECK(px(b, 40, 2, 33) == 0 && px(b, 40, 4, 33) == 0 && px(b, 40, 6, 33) == 1);
}

// Dots and ticks stay inside the bar at any size: rows 0 and h-1 blank, nothing written past the bitmap.
static void passMarksStayInsideTheBar()
{
    const struct { int w, h; } sz[] = { {16, 8}, {8, 8}, {40, 3} };
    for (auto s : sz)
        for (bool recording : {true, false})
        {
            auto v = view(); v.loopLength = 1000;
            if (recording) { v.state[0] = TrackState::Recording; v.trackPosition = 30000; }   // 30 passes: dots
            else { v.state[0] = TrackState::Playing; v.trackLength = 3000; }                   // 3 loops: ticks
            const size_t bytes = size_t((s.w + 7) / 8) * s.h;
            uint8_t b[64]; std::memset(b, 0xA5, sizeof b);
            ouro::renderLoopWidget(v, s.w, s.h, b);
            bool canary = true; for (size_t i = bytes; i < sizeof b; ++i) canary &= b[i] == 0xA5;
            CHECK(canary);
            CHECK(litIn(b, s.w, 0, 0, s.w, 1) == 0 && litIn(b, s.w, 0, s.h - 1, s.w, s.h) == 0);
        }
}
static void passDotsStopAt18()
{
    auto v = view(); v.state[0] = TrackState::Recording; v.loopLength = 1000; v.trackPosition = 30000;
    uint8_t b[1024]; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(litIn(b, 40, 1, 33, 39, 34) == 18);
}

// A long track emptied by undoing its only recording keeps its length in the engine (for a redo); the bar shows the
// first loop, like a track never recorded: no ticks, the fill follows position/loopLength.
static void emptyTrackShowsTheLoopNotItsOldLength()
{
    auto v = view(); v.state[0] = TrackState::Empty; v.loopLength = 1000; v.position = 500;
    v.trackLength = 3000; v.trackPosition = 0;
    uint8_t b[1024]; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(px(b, 40, 14, 29) == 1 && px(b, 40, 26, 29) == 0);         // no inverted ticks in the filled or empty part
    int lit = 0; for (int x = 1; x < 39; ++x) lit += px(b, 40, x, 30);
    CHECK(std::abs(lit - 19) <= 1);                                    // half of the first loop, not 0 of 3000
}

static bool sameBits(const uint8_t* a, const uint8_t* b, size_t n) { return std::memcmp(a, b, n) == 0; }

static void loopWidgetShowsPartQueuedSongAndFull()
{
    uint8_t a[1024], b[1024];
    auto v = view(); v.part = 0; ouro::renderLoopWidget(v, 40, 37, a);
    v.part = 1; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(!sameBits(a, b, 5 * 37));
    uint8_t c[1024]; v.part = 2; ouro::renderLoopWidget(v, 40, 37, c);
    CHECK(!sameBits(b, c, 5 * 37));                                   // each part has its own letter
    v = view(); ouro::renderLoopWidget(v, 40, 37, a);
    v.queuedPart = 2; v.blinkOn = true; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(!sameBits(a, b, 5 * 37));
    uint8_t off[1024]; v.blinkOn = false; ouro::renderLoopWidget(v, 40, 37, off);
    CHECK(!sameBits(b, off, 5 * 37));                                 // A>B blinks
    v = view(); v.state[0] = TrackState::Playing; ouro::renderLoopWidget(v, 40, 37, a);
    v.songStep = 2; v.songLength = 6; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(!sameBits(a, b, 5 * 37));
    v.songStep = 3; ouro::renderLoopWidget(v, 40, 37, a);
    CHECK(!sameBits(a, b, 5 * 37));                                   // the position changes the picture
    v = view(); v.state[0] = TrackState::Playing; ouro::renderLoopWidget(v, 40, 37, a);
    v.memoryFull = true; ouro::renderLoopWidget(v, 40, 37, b);
    CHECK(!sameBits(a, b, 5 * 37));
    CHECK(showsText(v, "FULL"));
    v.queuedPart = 1; v.blinkOn = true; CHECK(showsText(v, "FULL"));   // FULL wins over A>B
    v = view(); v.queuedPart = 1; v.blinkOn = true; v.part = 0; CHECK(showsText(v, "A>B"));
    v = view(); v.songStep = 2; v.songLength = 6; v.state[0] = TrackState::Playing; CHECK(showsText(v, "3/6"));   // instead of PLAY
    v.state[0] = TrackState::Overdubbing; CHECK(showsText(v, "DUB"));                // recording feedback stays visible in song play
    v.state[0] = TrackState::Recording; v.queuedPart = 1; v.blinkOn = true; CHECK(showsText(v, "REC"));   // REC/DUB beat A>B
    v.state[0] = TrackState::Playing; CHECK(showsText(v, "A>B"));                    // A>B beats the position
    v.blinkOn = false; CHECK(showsText(v, "3/6"));                                   // ...and alternates with it
    v.queuedPart = -1; v.armed[0] = true; CHECK(showsText(v, "ARM"));                // ARM, WAIT and STOP beat the position
    v.armed[0] = false; v.waiting = true; CHECK(showsText(v, "WAIT"));
    v.waiting = false; v.transport = ouro::Transport::Stopped; CHECK(showsText(v, "STOP"));
    v.transport = ouro::Transport::Running; v.state[0] = TrackState::Empty; CHECK(showsText(v, "3/6"));   // the position replaces -- too
    v.songStep = -1; CHECK(showsText(v, "--")); v.songStep = 2;   // no song, no position
    v.songStep = 7; v.songLength = 2; CHECK(showsText(v, "2/2"));   // never past the length (a step before it was edited to Stop)
    v.songStep = 2; v.songLength = 6;
    v.state[0] = TrackState::Playing;
    v.songStep = 9; v.songLength = 16;   // 5 characters still fit in 40 px
    ouro::renderLoopWidget(v, 40, 37, a);
    CHECK(litIn(a, 40, 11, 2, 40, 9) > 0 && litIn(a, 40, 0, 0, 40, 2) == 0);
    // the marks band, the bar and rows below the text are not touched by any of it
    uint8_t p[1024], q[1024];
    v = view(); v.state[0] = TrackState::Playing; v.loopLength = 1000; v.position = 500; ouro::renderLoopWidget(v, 40, 37, p);
    v.part = 3; v.queuedPart = 0; v.songStep = 1; v.songLength = 4; v.memoryFull = true; v.blinkOn = true; ouro::renderLoopWidget(v, 40, 37, q);
    for (int y = 9; y < 37; ++y) for (int x = 0; x < 40; ++x) CHECK(px(p, 40, x, y) == px(q, 40, x, y));
}

static ouro::SongView sixSteps()
{
    ouro::SongView s = {};
    const uint8_t part[6] = { 0, 1, 0, 1, 2, ouro::kStepStop }, rep[6] = { 4, 2, 4, 4, 2, 1 };
    for (int i = 0; i < ouro::kSteps; ++i) { s.part[i] = i < 6 ? part[i] : 0; s.repeats[i] = i < 6 ? rep[i] : 7; }   // steps 6.. lie behind the Stop
    s.playing = -1; s.edited = 0; s.on = true;
    return s;
}

// 128 px: 6 cells of 21 px from x = 1; label rows 11..17, number rows 2..8
static void songWidgetDrawsEachStepsLabel()
{
    auto s = sixSteps(); uint8_t b[1024], ref[1024];
    CHECK(ouro::renderSongWidget(s, 128, 37, b) == 16 * 37);
    const char* lab[6] = { "AX4", "BX2", "AX4", "BX4", "CX2", "STOP" };
    for (int k = 0; k < 6; ++k)
    {
        std::memset(ref, 0, sizeof ref);
        const int lw = int(std::strlen(lab[k])) * 6 - 1, x0 = 1 + 21 * k;
        ouro::drawText(ref, 128, x0 + (21 - lw) / 2, 11, lab[k]);
        bool same = true;
        for (int y = 11; y < 18; ++y) for (int x = x0; x < x0 + 20; ++x) same &= px(b, 128, x, y) == px(ref, 128, x, y);
        CHECK(same && litIn(ref, 128, 0, 11, 128, 18) > 0);
        CHECK(litIn(b, 128, x0 - 1, 2, x0 + 22, 9) > 0);   // its number
    }
    CHECK(litIn(b, 128, 0, 0, 128, 1) == 0 && litIn(b, 128, 0, 36, 128, 37) == 0);
    CHECK(litIn(b, 128, 0, 29, 128, 36) > 0);              // the info line
}

static void songWidgetMarksPlayingAndEdited()
{
    auto s = sixSteps(); uint8_t a[1024], b[1024];
    s.edited = 1; s.playing = -1; ouro::renderSongWidget(s, 128, 37, a);
    CHECK(litIn(a, 128, 22, 20, 43, 23) == 0);                              // no playing mark
    CHECK(litIn(a, 128, 22, 24, 43, 27) > 0 && litIn(a, 128, 1, 24, 22, 27) == 0 && litIn(a, 128, 43, 24, 128, 27) == 0);   // edited: under cell 1 only
    s.playing = 3; ouro::renderSongWidget(s, 128, 37, b);
    CHECK(litIn(b, 128, 64, 20, 85, 23) == 9 && litIn(b, 128, 0, 20, 64, 23) == 0 && litIn(b, 128, 85, 20, 128, 23) == 0);   // a 5-3-1 triangle under cell 3
    s.on = false; ouro::renderSongWidget(s, 128, 37, b);
    CHECK(litIn(b, 128, 0, 20, 128, 23) == 0);                              // song off: nothing plays
    s.on = true; s.edited = 4; ouro::renderSongWidget(s, 128, 37, b);
    CHECK(litIn(b, 128, 85, 24, 106, 27) > 0 && litIn(b, 128, 22, 24, 43, 27) == 0);
}

static void songWidgetDrawsNothingBehindTheEnd()
{
    auto s = sixSteps(); uint8_t a[1024], b[1024];
    ouro::renderSongWidget(s, 128, 37, a);
    for (int i = 6; i < ouro::kSteps; ++i) { s.part[i] = 3; s.repeats[i] = 16; }
    ouro::renderSongWidget(s, 128, 37, b);
    CHECK(sameBits(a, b, 16 * 37));                                         // steps after the Stop are not drawn
    s.part[3] = ouro::kStepLoop;                                            // an earlier end: step 5 (CX2) and Stop vanish
    ouro::renderSongWidget(s, 128, 37, b);
    CHECK(litIn(b, 128, 64, 11, 128, 18) > 0 && litIn(b, 128, 86, 11, 128, 18) == 0);   // LOOP at step 3; nothing right of it
    CHECK(litIn(b, 128, 85, 2, 128, 9) == 0 && litIn(b, 128, 86, 11, 128, 18) == 0);
}

static void songWidgetNarrowStaysInItsBuffer()
{
    // a 96 x 16 area: 4 cells, the window follows the edited step; guard bytes behind the bitmap stay untouched
    for (int e = 0; e < ouro::kSteps; ++e)
        for (int end : { 5, 15 })
        {
            auto s = sixSteps(); s.edited = e; s.playing = e % 6;
            for (int i = 0; i < ouro::kSteps; ++i) { if (i > end) s.part[i] = 0; }
            if (end == 15) { for (int i = 0; i < 15; ++i) { s.part[i] = uint8_t(i % 4); s.repeats[i] = 16; } s.part[15] = ouro::kStepLoop; }
            uint8_t buf[12 * 16 + 64]; std::memset(buf, 0xA5, sizeof buf);
            CHECK(ouro::renderSongWidget(s, 96, 16, buf) == 12 * 16);
            bool guard = true; for (size_t i = 12 * 16; i < sizeof buf; ++i) guard &= buf[i] == 0xA5;
            CHECK(guard);
            CHECK(litIn(buf, 96, 0, 0, 96, 1) == 0 && litIn(buf, 96, 0, 15, 96, 16) == 0);
            CHECK(litIn(buf, 96, 0, 12, 96, 15) > 0);                       // the edited step is always in the window
        }
    for (int sz : { 96, 128 })   // the whole range of edited steps at the natural size too
        for (int e = 0; e < ouro::kSteps; ++e)
        {
            auto s = sixSteps(); s.edited = e; uint8_t buf[16 * 37 + 64]; std::memset(buf, 0xA5, sizeof buf);
            const size_t n = ouro::renderSongWidget(s, uint16_t(sz), 37, buf);
            bool guard = true; for (size_t i = n; i < sizeof buf; ++i) guard &= buf[i] == 0xA5;
            CHECK(guard);
        }
}

static ouro::PiecesView piecesView(const char* status)
{
    ouro::PiecesView v = {};
    v.full = 1u << 0 | 1u << 1 | 1u << 4; v.selected = 2; v.loaded = 1; v.status = status;   // slots 1, 2, 5 full; 3 selected; 2 loaded
    return v;
}

static bool sameRows(const uint8_t* a, const uint8_t* b, int w, int y0, int y1)
{
    for (int y = y0; y < y1; ++y) for (int x = 0; x < w; ++x) if (px(a, w, x, y) != px(b, w, x, y)) return false;
    return true;
}

static void newGlyphsDrawAndDiffer()
{
    const char* all = "VNGK?%";
    for (const char* c = all; *c; ++c)
    {
        const uint8_t* g = ouro::glyph(*c);
        int lit = 0; for (int r = 0; r < 7; ++r) lit += g[r] != 0;
        CHECK(lit > 0);
        CHECK(std::memcmp(g, ouro::glyph(' '), 7) != 0);
        for (const char* d = all; d < c; ++d) CHECK(std::memcmp(g, ouro::glyph(*d), 7) != 0);
    }
}

static void piecesWidgetLayout()
{
    uint8_t b[1024];
    CHECK(ouro::renderPiecesWidget(piecesView("SAVE 3?"), 128, 37, b) == 16 * 37);
    CHECK(litIn(b, 128, 5, 8, 10, 13) == 25);        // slot 1 filled box, rows 8-12
    CHECK(litIn(b, 128, 16 + 5, 8, 16 + 10, 13) == 25);                                  // slot 2 filled
    CHECK(litIn(b, 128, 32 + 6, 9, 32 + 9, 12) == 0 && litIn(b, 128, 32 + 5, 8, 32 + 10, 13) == 16);   // slot 3 outline
    CHECK(litIn(b, 128, 5, 23, 10, 28) == 16);                                           // slot 9 empty, second row at 23-27
    CHECK(litIn(b, 128, 5, 13, 10, 14) == 0 && litIn(b, 128, 16 + 5, 13, 16 + 10, 14) == 5);   // underline under the loaded slot 2 only
    CHECK(litIn(b, 128, 5, 28, 128, 29) == 0);
    // selected number 3 inverted in a 13x9 box (rows 0-7 visible, the box row above the screen), the others plain
    CHECK(litIn(b, 128, 32 + 1, 0, 32 + 14, 8) > 60 && litIn(b, 128, 16 + 1, 0, 16 + 14, 8) < 25);
    CHECK(px(b, 128, 32 + 1, 0) == 1 && px(b, 128, 32 + 13, 7) == 1 && px(b, 128, 32 + 14, 4) == 0);
    uint8_t ref[1024]; std::memset(ref, 0, sizeof ref); ouro::drawText(ref, 128, 0, 30, "SAVE 3?"); ouro::drawText(ref, 128, 128 - 47, 30, "LOADED 2");
    CHECK(sameRows(b, ref, 128, 30, 37));                                                // status at row 30, nothing else there
    // numbers 9-16 at rows 15-21, "10" centred in its cell
    std::memset(ref, 0, sizeof ref); ouro::drawText(ref, 128, 16 + 2, 15, "10");
    bool same = true;
    for (int y = 15; y < 22; ++y) for (int x = 16; x < 32; ++x) same &= px(b, 128, x, y) == px(ref, 128, x, y);
    CHECK(same);
    // the selected slot moves the inversion; an unselected number is just text
    auto v = piecesView(""); v.selected = 9; ouro::renderPiecesWidget(v, 128, 37, ref);
    CHECK(px(ref, 128, 16 + 1, 14) == 1 && px(ref, 128, 16 + 13, 22) == 1);
    CHECK(litIn(ref, 128, 32 + 1, 0, 32 + 14, 8) < 25);
}

static void piecesWidgetLoadedAndStatusFit()
{
    uint8_t b[1024], ref[1024];
    auto v = piecesView("3 EMPTY"); v.loaded = 1;
    ouro::renderPiecesWidget(v, 128, 37, b);
    std::memset(ref, 0, sizeof ref); ouro::drawText(ref, 128, 0, 30, "3 EMPTY"); ouro::drawText(ref, 128, 128 - 47, 30, "LOADED 2");
    CHECK(sameRows(b, ref, 128, 30, 37));
    v.loaded = -1; ouro::renderPiecesWidget(v, 128, 37, b);
    std::memset(ref, 0, sizeof ref); ouro::drawText(ref, 128, 0, 30, "3 EMPTY");
    CHECK(sameRows(b, ref, 128, 30, 37));                                                // nothing loaded: no LOADED
    CHECK(litIn(b, 128, 0, 13, 128, 14) == 0);                                           // and no underline
    // fallback at 96x16: fits ("3 EMPTY" + "LOADED 2": 49 >= 48), and a long status leaves LOADED out
    v.loaded = 1; ouro::renderPiecesWidget(v, 96, 16, b);
    std::memset(ref, 0, sizeof ref); ouro::drawText(ref, 96, 0, 1, "3 EMPTY"); ouro::drawText(ref, 96, 96 - 47, 1, "LOADED 2");
    CHECK(sameRows(b, ref, 96, 0, 16));
    v.status = "120 BPM 4 BARS"; ouro::renderPiecesWidget(v, 96, 16, b);
    std::memset(ref, 0, sizeof ref); ouro::drawText(ref, 96, 0, 1, "120 BPM 4 BARS");
    CHECK(sameRows(b, ref, 96, 0, 16));
    v.status = "1 BAR"; v.loaded = 15; ouro::renderPiecesWidget(v, 128, 37, b);           // "LOADED 16" is 53 px
    std::memset(ref, 0, sizeof ref); ouro::drawText(ref, 128, 0, 30, "1 BAR"); ouro::drawText(ref, 128, 128 - 53, 30, "LOADED 16");
    CHECK(sameRows(b, ref, 128, 30, 37));
}

static void piecesWidgetSmallAreas()
{
    uint8_t b[1024];
    auto v = piecesView("SAVE 3?"); v.loaded = -1;
    CHECK(ouro::renderPiecesWidget(v, 96, 16, b) == 12 * 16);
    uint8_t ref[1024]; std::memset(ref, 0, sizeof ref); ouro::drawText(ref, 96, 0, 1, "SAVE 3?");
    CHECK(std::memcmp(b, ref, 12 * 16) == 0);                                            // only the status, at row 1
    CHECK(ouro::renderPiecesWidget(v, 96, 15, b) == 12 * 15);
    CHECK(litIn(b, 96, 0, 0, 96, 15) == 0);                                              // below 16 rows: blank
    uint8_t buf[12 * 16 + 8]; std::memset(buf, 0xA5, sizeof buf);
    ouro::renderPiecesWidget(v, 96, 16, buf);
    for (size_t i = 12 * 16; i < sizeof buf; ++i) CHECK(buf[i] == 0xA5);                 // guard bytes untouched
    for (int h = 16; h < 37; ++h)
        for (int w : {96, 100, 128}) { uint8_t t[1024]; CHECK(ouro::renderPiecesWidget(v, uint16_t(w), uint16_t(h), t) == size_t((w + 7) / 8) * h); }
    // narrower than the grid's 128 columns (piece_slots' minimum is 96): only the status, as below 37 rows
    for (int w : {96, 127}) {
        const int bpr = (w + 7) / 8;
        CHECK(ouro::renderPiecesWidget(v, uint16_t(w), 37, b) == size_t(bpr) * 37);
        std::memset(ref, 0, sizeof ref); ouro::drawText(ref, w, 0, 1, "SAVE 3?");
        CHECK(std::memcmp(b, ref, size_t(bpr) * 37) == 0);
    }
}

static void loopWidgetShowsLoad()
{
    auto v = view(); v.loading = true; v.memoryFull = true; v.state[0] = TrackState::Playing;
    CHECK(showsText(v, "LOAD"));                                                         // over FULL too
    v.loading = false; CHECK(showsText(v, "FULL"));
}

int main()
{
    sizeIsMono1(); barFillsProportionally(); emptyBarWhenNoLoop(); elapsedShownWhileRecording();
    marksReflectTracks(); selectedFrame(); ringsAroundSymbols(); stateText(); stateTextNotClipped(); tallThreshold(); layoutRows(); shortAreaBarOnly(); topAndBottomRowsBlank();
    stoppedShowsStop(); armedShowsArmAndBlinksItsDot(); everyWidgetStringHasGlyphs(); waitShowsWaitInPlaceOfTheState();
    countsUnderTheirMarks(); countsAbove99Show99(); countRowsDontOverlap(); noCountsWhenShort();
    longTrackBarHasPassTicks(); crowdedTicksAreLeftOut(); recordingShowsPassesSoFar(); passMarksStayInsideTheBar(); passDotsStopAt18();
    emptyTrackShowsTheLoopNotItsOldLength();
    loopWidgetShowsPartQueuedSongAndFull(); songWidgetDrawsEachStepsLabel(); songWidgetMarksPlayingAndEdited();
    songWidgetDrawsNothingBehindTheEnd(); songWidgetNarrowStaysInItsBuffer();
    newGlyphsDrawAndDiffer(); piecesWidgetLayout(); piecesWidgetLoadedAndStatusFit(); piecesWidgetSmallAreas(); loopWidgetShowsLoad();
    REPORT();
}
