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

// Drives a Looper with a virtual MOD transport: before each block it hands the engine the grid at that block start.
// toPhase() hands it too, so a press right after it sees the grid even with no block between (the plugin hands the
// grid before the presses); grid() alone for a press before any block.
struct SyncRig : Rig {
    bool valid = true, playing = true; double spb = 96000.0, phase = 0.0;   // 120 BPM 4/4 at 48 kHz; phase: samples into the bar
    using Rig::Rig;
    void grid() { lp.setGrid(valid, playing, spb, phase == 0.0 ? 0.0 : spb - phase); }
    void feedSync(const std::vector<float>& in) {
        std::vector<float> o(block);
        for (size_t i = 0; i < in.size(); i += block) {
            const uint32_t n = uint32_t(std::min<size_t>(block, in.size() - i));
            grid();
            lp.process(in.data() + i, o.data(), n);
            out.insert(out.end(), o.begin(), o.begin() + n);
            if (playing) phase = std::fmod(phase + n, spb);
        }
    }
    void silenceSync(uint32_t n) { feedSync(std::vector<float>(n, 0.0f)); }
    void toPhase(double p) { silenceSync(uint32_t(std::fmod(p - phase + 2 * spb, spb))); grid(); }   // block-quantized is fine at 128 | p
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
