// Renders the loop widget (state text, track marks, progress bar / elapsed time) as a Mono1 bitmap.
#pragma once
#include "Font.hpp"
#include "../dsp/Looper.hpp"

#include <cstdio>
#include <cstring>

namespace ouro {

struct WidgetView {
    int selected; TrackState state[kTracks]; bool on[kTracks];
    uint32_t loopLength, position, recordedSamples; double sampleRate; bool blinkOn;
};

// Mono1: rows top->bottom, (w+7)/8 bytes per row, MSB = leftmost pixel, 1 = lit. Returns bytes written.
inline size_t renderLoopWidget(const WidgetView& v, uint16_t w, uint16_t h, uint8_t* bits)
{
    const size_t bytes = size_t((w + 7) / 8) * h;
    std::memset(bits, 0, bytes);
    const auto rect = [&](int x0, int y0, int x1, int y1, bool fill) {   // [x0,x1) x [y0,y1)
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                if (fill || x == x0 || x == x1 - 1 || y == y0 || y == y1 - 1) setPixel(bits, w, x, y);
    };

    const auto clearPixel = [&](int x, int y) { bits[y * ((w + 7) / 8) + x / 8] &= uint8_t(~(0x80 >> (x % 8))); };

    // Rows 0 and h-1 stay blank (padding). Tall: text row 1.., marks rows 11..17, 7-row bar at rows h-8..h-2.
    // Below 26 rows the bar (starting at h-8) would overlap the marks (ending at row 17), so use the short layout:
    // the bar alone at rows 1..h-2.
    const bool tall = h >= 26;
    const int barY = tall ? h - 8 : 1, barH = h - 1 - barY;

    if (tall)
    {
        static const char* const names[] = { "--", "REC", "PLAY", "DUB" };   // by TrackState: Empty, Recording, Playing, Overdubbing
        char t[3] = { 'T', char('1' + v.selected), '\0' };
        drawText(bits, w, 0, 1, t);
        drawText(bits, w, 14, 1, names[int(v.state[v.selected])]);

        const int cell = w / kTracks;
        for (int i = 0; i < kTracks; ++i)
        {
            const int x = i * cell + (cell - 7) / 2;
            const TrackState s = v.state[i];
            if (s == TrackState::Empty) rect(x + 2, 13, x + 5, 16, true);
            else if (s == TrackState::Playing || v.blinkOn) rect(x, 11, x + 7, 18, v.on[i]);
        }
    }

    // Bar: outline, inside filled to the position; or just the elapsed time while the first loop records.
    bool rec = false;
    for (int i = 0; i < kTracks; ++i) rec |= v.state[i] == TrackState::Recording;
    if (v.loopLength == 0 && rec)
    {
        const unsigned secs = v.sampleRate > 0 ? unsigned(v.recordedSamples / v.sampleRate) : 0;
        char s[8];
        std::snprintf(s, sizeof s, "%u:%02u", (secs / 60) % 10, secs % 60);
        drawText(bits, w, 1, barY + (barH - 7) / 2, s);
        // the 7-row font does not fit a 6-row bar area (h = 8): clip it so the padding rows stay blank
        for (int x = 0; x < w; ++x) { clearPixel(x, 0); clearPixel(x, h - 1); }
        return bytes;
    }
    rect(0, barY, w, h - 1, false);
    if (v.loopLength > 0)
    {
        const uint64_t inner = w > 2 ? w - 2 : 0;
        uint64_t fill = (inner * v.position + v.loopLength / 2) / v.loopLength;
        if (fill > inner) fill = inner;
        if (fill > 0) rect(1, barY + 1, 1 + int(fill), h - 2, true);
    }
    return bytes;
}

} // namespace ouro
