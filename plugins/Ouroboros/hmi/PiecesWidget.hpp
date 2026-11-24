// Renders the pieces widget (16 slots: number, full/empty box, the selected and the loaded one marked; a status line) as a Mono1 bitmap.
#pragma once
#include "Font.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ouro {

struct PiecesView {
    uint16_t full;         // bit s: slot s+1 holds a piece
    int selected;          // 0-15
    int loaded;            // 0-15, -1: none
    const char* status;    // drawn as given
};

// Mono1 like renderSongWidget. From 37 rows: two rows of 8 cells 16 px wide (number at rows 0-6 / 15-21, a 5x5 box 2 rows
// under it: filled = holds a piece, outline = empty); the selected number inverted in a 13x9 box (1 row above the number
// to 1 below it, clipped at the top edge), the loaded slot's box underlined one row below it; the status line at rows 30-36
// with "LOADED M" right-aligned when it fits after the status. Below 37 rows only the status line (row 1); below 16 nothing.
inline size_t renderPiecesWidget(const PiecesView& v, uint16_t w, uint16_t h, uint8_t* bits)
{
    const size_t bytes = size_t((w + 7) / 8) * h;
    std::memset(bits, 0, bytes);
    if (h < 16) return bytes;
    const bool full = h >= 37;

    if (full)
        for (int s = 0; s < 16; ++s)
        {
            const int x0 = s % 8 * 16, ny = s < 8 ? 0 : 15, by = ny + 8;
            char n[4];
            std::snprintf(n, sizeof n, "%d", s + 1);
            drawText(bits, w, x0 + (16 - (int(std::strlen(n)) * 6 - 1)) / 2, ny, n);
            if (s == v.selected)   // invert the 13x9 box round the number
                for (int y = std::max(ny - 1, 0); y < ny + 8; ++y)
                    for (int x = x0 + 1; x < x0 + 14 && x < w; ++x) bits[y * ((w + 7) / 8) + x / 8] ^= uint8_t(0x80 >> (x % 8));
            for (int y = 0; y < 5; ++y)
                for (int x = 0; x < 5; ++x)
                    if (v.full >> s & 1 || x == 0 || x == 4 || y == 0 || y == 4) setPixel(bits, w, x0 + 5 + x, by + y);
            if (s == v.loaded)
                for (int x = 0; x < 5; ++x) setPixel(bits, w, x0 + 5 + x, by + 5);
        }

    const int y = full ? 30 : 1, len = int(std::strlen(v.status));
    drawText(bits, w, 0, y, v.status);
    if (v.loaded >= 0)
    {
        char l[24];
        std::snprintf(l, sizeof l, "LOADED %d", v.loaded + 1);
        const int x = w - (int(std::strlen(l)) * 6 - 1);
        if (x >= (len + 1) * 6) drawText(bits, w, x, y, l);
    }
    return bytes;
}

} // namespace ouro
