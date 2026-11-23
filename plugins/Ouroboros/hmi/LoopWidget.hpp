// Renders the loop widget (state text, track marks and layer counts, progress bar / elapsed time) as a Mono1 bitmap.
#pragma once
#include "Font.hpp"
#include "../dsp/Looper.hpp"

#include <cstdio>
#include <cstring>

namespace ouro {

struct WidgetView {
    int selected; TrackState state[kTracks]; bool on[kTracks];
    uint32_t loopLength, position, recordedSamples; double sampleRate; bool blinkOn;
    Transport transport; bool armed[kTracks]; bool waiting;   // waiting: a user action waits for the bar line
    uint32_t trackLength, trackPosition;   // selected track: length (0 = none or still recording onto the loop), samples since its start
    uint32_t layers[kTracks];              // layers heard per track (Looper::layerCount); 0 draws no count
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

    const auto drawMark = [&](int x, int y, const uint8_t* g) {   // 7x7 glyph, one row below the frame top
        for (int r = 0; r < 7; ++r)
            for (int c = 0; c < 7; ++c)
                if (g[r] & (0x40 >> c)) setPixel(bits, w, x + c, y + r);
    };

    const auto clearPixel = [&](int x, int y) { bits[y * ((w + 7) / 8) + x / 8] &= uint8_t(~(0x80 >> (x % 8))); };

    // Rows 0 and h-1 stay blank. Tall: text (7 rows), marks band (9-row frame, 7x7 marks inside) and 7-row bar, with the
    // free rows split into 4 gaps (top, text->marks, marks->bar, bottom): remainder to the two middle gaps first, then the bottom.
    // From 33 rows the layer counts (5 rows) sit 1 row under the frame and the marks->bar gap follows them; below, no counts.
    // Below 27 rows the four gaps would drop under 1 row, so use the short layout: the bar alone at rows 1..h-2.
    const bool tall = h >= 27, counts = h >= 33;
    const int freeRows = h - (counts ? 29 : 23), gap = freeRows / 4, extra = freeRows % 4;
    const int textY = gap, frameY = textY + 7 + gap + (extra > 0), tallBarY = frameY + (counts ? 15 : 9) + gap + (extra > 1);
    const int barY = tall ? tallBarY : 1, barEnd = tall ? barY + 7 : h - 1, barH = barEnd - barY;

    if (tall)
    {
        static const char* const names[] = { "--", "REC", "PLAY", "DUB" };   // by TrackState: Empty, Recording, Playing, Overdubbing
        char t[3] = { 'T', char('1' + v.selected), '\0' };
        drawText(bits, w, 0, textY, t);
        const char* txt = names[int(v.state[v.selected])];
        if (v.transport == Transport::Stopped) txt = "STOP";
        else if (v.transport == Transport::Stopping) txt = v.blinkOn ? "STOP" : "";
        else if (v.armed[v.selected]) txt = "ARM";
        if (v.waiting) txt = "WAIT";
        drawText(bits, w, 14, textY, txt);

        const int cell = w / kTracks;
        for (int i = 0; i < kTracks; ++i)
        {
            const int x = i * cell + (cell - 7) / 2;
            const TrackState s = v.state[i];
            if (i == v.selected) rect(x - 1, frameY, x + 8, frameY + 9, false);   // 9x9 frame round the selected mark, always drawn
            if (s == TrackState::Empty) { if (!v.armed[i] || v.blinkOn) rect(x + 2, frameY + 3, x + 5, frameY + 6, true); }
            else if (s == TrackState::Playing)   // play triangle = on, stop square = muted
                drawMark(x, frameY + 1, v.on[i] ? kMarkPlay : kMarkStop);
            else if (v.blinkOn) drawMark(x, frameY + 1, kMarkDisc);   // recording / overdubbing: blinking disc
            if (counts && v.layers[i]) drawCount(bits, w, x, frameY + 10, v.layers[i]);
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
    rect(0, barY, w, barEnd, false);
    if (v.loopLength > 0)
    {
        const uint64_t inner = w > 2 ? w - 2 : 0;
        // A long track fills by its own length; otherwise by the loop (or the base phase while recording onto it). An
        // empty track (its only recording undone, the length kept for a redo) shows like one never recorded: the loop.
        const uint64_t trackLen = v.state[v.selected] != TrackState::Empty ? v.trackLength : 0;
        const uint64_t len = trackLen > 0 ? trackLen : v.loopLength;
        const uint64_t pos = trackLen > 0 ? v.trackPosition : v.position;
        uint64_t fill = (inner * pos + len / 2) / len;
        if (fill > inner) fill = inner;
        if (fill > 0) rect(1, barY + 1, 1 + int(fill), barEnd - 1, true);
        const uint64_t k = trackLen / v.loopLength;
        // Ticks and dots need an inner row (barH >= 3). Ticks stay in the inner columns by construction (inner / k >= 3).
        if (k > 1 && inner / k >= 3 && barH >= 3)   // a tick per loop boundary: inverts the top inner row
            for (uint64_t i = 1; i < k; ++i)
            {
                const int x = 1 + int((2 * inner * i + k) / (2 * k));
                bits[(barY + 1) * ((w + 7) / 8) + x / 8] ^= uint8_t(0x80 >> (x % 8));
            }
        if (v.trackLength == 0 && v.state[v.selected] == TrackState::Recording && barH >= 3)   // passes so far: inverting dots in the bottom inner row
            for (uint64_t i = 0, n = v.trackPosition / v.loopLength; i < n && i < 18 && 2 + 2 * int(i) < w - 1; ++i)   // inner columns only
            {
                const int x = 2 + 2 * int(i);
                bits[(barEnd - 2) * ((w + 7) / 8) + x / 8] ^= uint8_t(0x80 >> (x % 8));
            }
    }
    return bytes;
}

} // namespace ouro
