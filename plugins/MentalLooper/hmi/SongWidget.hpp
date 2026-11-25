// Renders the song widget (the arrangement: step numbers, part x repeats, the playing step, the edited step) as a Mono1 bitmap.
#pragma once
#include "Font.hpp"
#include "../dsp/Constants.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ouro {

struct SongView {
    uint8_t part[kSteps];      // 0-3 = A-D, kStepLoop, kStepStop
    uint8_t repeats[kSteps];   // 1-16
    int playing;               // the step playing (-1: none)
    int edited;                // the step the Step port selects, 0-15
    bool on;                   // Song on: no playing mark while off
};

// Mono1 like renderLoopWidget. Steps sit in 21 px cells (a 3-4 character label; "STOP" and "BX16" overhang 1 px each side
// into the cell margins); steps after the first Loop/Stop are not drawn, that one is. A narrow area shows the cells that
// fit, the window following the edited step. Marks: a triangle under the playing step, a bracket under the edited one.
// From 37 rows a number row sits above the labels and an info line ("STEP 4 PART B X4") below.
inline size_t renderSongWidget(const SongView& v, uint16_t w, uint16_t h, uint8_t* bits)
{
    const size_t bytes = size_t((w + 7) / 8) * h;
    std::memset(bits, 0, bytes);
    if (h < 16) return bytes;   // below the minimum area: nothing fits
    const int cw = 21;
    const bool full = h >= 37;
    const int numY = 2, labY = full ? 11 : 1, playY = full ? 20 : 9, editY = full ? 24 : 12, infoY = 29;

    const auto label = [&](int i, char* s) {
        if (v.part[i] == kStepLoop) std::strcpy(s, "LOOP");
        else if (v.part[i] == kStepStop) std::strcpy(s, "STOP");
        else std::snprintf(s, 8, "%cX%u", 'A' + v.part[i] % kParts, unsigned(v.repeats[i]));
    };

    int count = 0;   // steps drawn: up to and including the first Loop/Stop
    while (count < kSteps && v.part[count] < kParts) ++count;
    count = std::min(count + 1, int(kSteps));
    const int edited = std::min(std::max(v.edited, 0), kSteps - 1);
    const int limit = std::max(count, edited + 1);   // the edited step past the end still gets its mark
    const int nvis = std::max(1, std::min(int(w) / cw, limit));
    const int first = std::min(std::max(edited - nvis / 2, 0), limit - nvis);
    const int pad = std::max(0, (int(w) - int(w) / cw * cw) / 2);   // cells keep their place however few are drawn

    for (int k = 0; k < nvis; ++k)
    {
        const int i = first + k, x0 = pad + k * cw;
        if (i < count)
        {
            char s[8], n[12];
            label(i, s);
            std::snprintf(n, sizeof n, "%d", i + 1);
            const int lw = int(std::strlen(s)) * 6 - 1, nw = int(std::strlen(n)) * 6 - 1;
            if (full) drawText(bits, w, x0 + (cw - nw) / 2, numY, n);
            drawText(bits, w, x0 + (cw - lw) / 2, labY, s);
        }
        const int cx = x0 + cw / 2;
        if (v.on && i == v.playing)   // triangle pointing up, 5 wide
            for (int r = 0; r < 3; ++r)
                for (int c = -(2 - r); c <= 2 - r; ++c) setPixel(bits, w, cx + c, playY + r);
        if (i == edited)   // bracket: a line under the cell with an upturned end each side
        {
            for (int x = x0 + 1; x < x0 + cw - 1; ++x) setPixel(bits, w, x, editY + 2);
            for (int r = 0; r < 2; ++r) { setPixel(bits, w, x0 + 1, editY + r); setPixel(bits, w, x0 + cw - 2, editY + r); }
        }
    }

    if (full)
    {
        char s[40];
        const int p = v.part[edited];
        if (p >= kParts) std::snprintf(s, sizeof s, "STEP %d %s", edited + 1, p == kStepLoop ? "LOOP" : "STOP");
        else std::snprintf(s, sizeof s, "STEP %d PART %c X%u", edited + 1, 'A' + p, unsigned(v.repeats[edited]));
        drawText(bits, w, 0, infoY, s);
    }
    return bytes;
}

} // namespace ouro
