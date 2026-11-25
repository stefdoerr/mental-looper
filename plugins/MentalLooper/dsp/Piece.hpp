#pragma once
#include <cstdint>
#include "dsp/Constants.hpp"
#include "dsp/BlockPool.hpp"

namespace ouro {
// What a saved piece holds besides the audio. No I/O here.
struct PieceTrack { uint32_t len = 0; float volDb = 0.0f; bool on = true; };
struct PieceMeta {
    uint32_t rate = 0, loop = 0, bars = 1;     // sample rate, L, the first loop's bars
    float bpm = 0.0f;                          // 0: unknown
    int part = 0;                              // 0-3
    bool song = false;
    int8_t stepPart[kSteps];                   // as Looper::setStep: 0-3, kStepLoop, kStepStop (default A, then Loop)
    uint8_t stepReps[kSteps];                  // 1-16 (default 1)
    PieceTrack track[kParts][kTracks];
    PieceMeta() {
        for (int i = 0; i < kSteps; ++i) { stepPart[i] = int8_t(i == 0 ? 0 : kStepLoop); stepReps[i] = 1; }
    }
};
inline uint32_t blocksFor(const PieceMeta& m) {
    uint32_t n = 0;
    for (int p = 0; p < kParts; ++p)
        for (int t = 0; t < kTracks; ++t)
            n += (m.track[p][t].len + BlockPool::kBlock - 1) / BlockPool::kBlock;
    return n;
}
}
