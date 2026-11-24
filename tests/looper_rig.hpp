#pragma once
#include "signals.hpp"
#include "test_util.hpp"
#include "dsp/Looper.hpp"
#include <vector>

// Drives a Looper in block-sized process() calls and records every output sample.
struct Rig {                                         // trigger latency forced to 0 unless a test sets it
    ouro::Looper lp; double sr; uint32_t block; std::vector<float> out;

    explicit Rig(double rate = 48000.0, uint32_t blockSize = 128, size_t poolBytes = 0) : sr(rate), block(blockSize) {
        lp.init(sr, block, poolBytes);              // 0: the default pool
        lp.setTriggerLatencyMs(0.0f);
    }
    void feed(const std::vector<float>& in) {        // block-sized process() calls; appends to out
        std::vector<float> o(block);
        for (size_t i = 0; i < in.size(); i += block) {
            const uint32_t n = uint32_t(std::min<size_t>(block, in.size() - i));
            lp.process(in.data() + i, o.data(), n);
            out.insert(out.end(), o.begin(), o.begin() + n);
        }
    }
    void silence(uint32_t n) { feed(std::vector<float>(n, 0.0f)); }
    std::vector<float> capture(uint32_t n) {         // feed n zeros, return those outputs
        const size_t from = out.size();
        silence(n);
        return slice(out, from, out.size());
    }
    std::vector<float> loopImage() {                 // one pass of silence, indexed by loop position
        const uint32_t L = lp.loopLength();
        std::vector<float> img(L);
        if (L == 0) return img;
        const uint32_t p0 = lp.position();
        auto o = capture(L);
        for (uint32_t i = 0; i < L; ++i) img[(p0 + i) % L] = o[i];
        return img;
    }
    std::vector<float> settle() {                    // one pass of silence, then loopImage()
        silence(std::max(lp.loopLength(), block));
        return loopImage();
    }
};

inline float fadeInGain(uint32_t i, double sr = 48000.0) {   // linear 0->1 over kFadeInMs
    const uint32_t f = ouro::msToSamples(ouro::kFadeInMs, sr);
    return i < f ? float(i) / float(f) : 1.0f;
}

// Track 0: press, sine(220, amp, n), press; returns loopImage().
inline std::vector<float> recordBase(Rig& r, uint32_t n, float amp) {
    r.lp.pressRec(0); r.feed(sine(220, amp, n)); r.lp.pressRec(0);
    return r.loopImage();
}

// Track t, with L set: press, one pass of sine(hz, amp), press.
inline void recordBaseOn(Rig& r, int t, float amp = 0.1f, double hz = 550.0) {
    r.lp.pressRec(t); r.feed(sine(hz, amp, r.lp.loopLength())); r.lp.pressRec(t);
}
