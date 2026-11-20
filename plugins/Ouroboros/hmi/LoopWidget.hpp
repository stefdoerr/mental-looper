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

    const bool tall = h >= 20;
    const int barH = tall ? 7 : h, barY = h - barH;

    if (tall)
    {
        static const char* const names[] = { "--", "REC", "PLAY", "DUB" };   // by TrackState: Empty, Recording, Playing, Overdubbing
        char t[3] = { 'T', char('1' + v.selected), '\0' };
        drawText(bits, w, 0, 0, t);
        drawText(bits, w, 18, 0, names[int(v.state[v.selected])]);

        const int cell = w / kTracks;
        for (int i = 0; i < kTracks; ++i)
        {
            const int x = i * cell + (cell - 7) / 2;
            const TrackState s = v.state[i];
            if (s == TrackState::Empty) rect(x + 2, 12, x + 5, 15, true);
            else if (s == TrackState::Playing || v.blinkOn) rect(x, 10, x + 7, 17, v.on[i]);
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
        return bytes;
    }
    rect(0, barY, w, h, false);
    if (v.loopLength > 0)
    {
        const uint64_t inner = w > 2 ? w - 2 : 0;
        uint64_t fill = (inner * v.position + v.loopLength / 2) / v.loopLength;
        if (fill > inner) fill = inner;
        if (fill > 0) rect(1, barY + 1, 1 + int(fill), h - 1, true);
    }
    return bytes;
}

} // namespace ouro
