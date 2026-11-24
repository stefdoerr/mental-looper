// CPU bench: time Looper::process() over a 4-track session. Build once against main's engine, once against the
// tempo-sync branch's. SYNC=0: the plugin with Sync Off (no setSync/setGrid calls). SYNC=1: Sync = Bars, MOD playing.
#include "dsp/Looper.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#ifndef SYNC
#define SYNC 0
#endif

int main() {
    const double sr = 48000.0; const uint32_t B = 128;
    const double spb = 96000.0;                       // 120 BPM 4/4
    double modPos = 0.0;
    ouro::Looper lp; lp.init(sr, B); lp.setTriggerLatencyMs(0.0f);
#if SYNC
    lp.setSync(true);
#endif
    std::vector<float> in(B), out(B);
    uint64_t n = 0;
    auto block = [&] {
        for (uint32_t i = 0; i < B; ++i) in[i] = 0.1f * std::sin(2.0 * M_PI * 220.0 * double(n + i) / sr);
#if SYNC
        const double ph = std::fmod(modPos, spb);
        lp.setGrid(true, true, spb, ph == 0.0 ? 0.0 : spb - ph);
#endif
        lp.process(in.data(), out.data(), B);
        n += B; modPos += B;
    };
    auto run = [&](double secs) { for (uint64_t k = 0, m = uint64_t(secs * sr / B); k < m; ++k) block(); };

    // Session: track 0 records 2 bars, tracks 1-3 record onto the loop, then 3 dubs on each track.
    lp.pressRec(0); run(4.0); lp.pressRec(0); run(0.5);
    for (int t = 1; t < 4; ++t) { lp.pressRec(t); run(4.0); lp.pressRec(t); run(0.2); }
    for (int d = 0; d < 3; ++d)
        for (int t = 0; t < 4; ++t) { lp.pressRec(t); run(2.0); lp.pressRec(t); run(0.2); }

    double best = 1e9;
    for (int rep = 0; rep < 7; ++rep) {
        const auto a = std::chrono::steady_clock::now();
        run(30.0);                                    // 30 s playing 4 tracks
        lp.pressRec(rep % 4); run(4.0); lp.pressRec(rep % 4);   // plus an overdub
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
        if (s < best) best = s;
    }
    std::printf("SYNC=%d best of 7: %.4f s for 34 s of audio (%.3f%% of real time) out=%g\n",
                SYNC, best, 100.0 * best / 34.0, double(out[0]));
}
