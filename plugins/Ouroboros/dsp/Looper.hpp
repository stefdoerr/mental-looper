#pragma once
#include "BlockPool.hpp"
#include "Constants.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ouro {

enum class TrackState : uint8_t { Empty, Recording, Playing, Overdubbing };
enum class NextAction : uint8_t { Rec, Play, Dub };

// The looper engine. init() is the only allocating call (non-RT). process() and the
// controls are RT-safe and single-threaded: controls are called between process() calls
// and take effect at the start of the next one.
class Looper {
public:
    Looper() = default;
    Looper(const Looper&) = delete;
    Looper& operator=(const Looper&) = delete;
    ~Looper() { release(); }

    // False => disabled: process() outputs silence and every control is ignored.
    bool init(double sampleRate, uint32_t /*maxBlock: the engine runs per sample, no block scratch*/) {
        release();
        // Positions are uint32_t: reject rates whose reservation would not fit (also NaN and absurdly low rates).
        const double bufLen = (double(kMaxLoopSeconds) + 1.0) * sampleRate;   // 5 min + 1 s slack (tail, history)
        if (!(sampleRate >= 1000.0) || bufLen > double(UINT32_MAX)) return false;
        sr_ = sampleRate;
        maxRec_ = msToSamples(kMaxLoopSeconds * 1000.0f, sr_);
        minLoop_ = msToSamples(kMinLoopMs, sr_);
        fadeIn_ = msToSamples(kFadeInMs, sr_);
        histLen_ = msToSamples(kHistoryMs, sr_);
        for (Track& tr : tracks_)
            if (!(tr.buf = static_cast<float*>(std::calloc(size_t(bufLen), sizeof(float))))) return fail();
        if (!(hist_ = static_cast<float*>(std::calloc(histLen_, sizeof(float))))) return fail();
        size_t mb = size_t(kMaxBudgetMb);
        while (!pool_.reserve(mb << 20)) {
            if (mb == 16) return fail();
            mb = std::max<size_t>(mb / 2, 16);
        }
        pool_.setBudgetBytes(size_t(200) << 20);   // reserve() reset it to everything; 200 MB is the default
        histW_ = 0;
        enabled_ = true;
        reset();
        return true;
    }

    void reset() {
        for (Track& tr : tracks_) { tr.state = TrackState::Empty; tr.tailEnd = 0; }
        L_ = pos_ = rec_ = recD_ = 0;
        recTrack_ = -1;
    }

    void process(const float* in, float* out, uint32_t n) {
        if (!enabled_) { std::memset(out, 0, n * sizeof(float)); return; }
        for (uint32_t i = 0; i < n; ++i) {
            const float x = in[i];                       // read first: in and out may alias
            hist_[histW_] = x;
            if (++histW_ == histLen_) histW_ = 0;
            if (recTrack_ >= 0 && rec_ == maxRec_) closeFirst();   // max length: close as if B were pressed
            float y = 0.0f;
            for (Track& tr : tracks_) {
                switch (tr.state) {
                case TrackState::Recording:              // first recording: overwrite (reused pages hold old audio)
                    tr.buf[rec_] = x * fadeInGain(rec_);
                    ++rec_;
                    break;
                case TrackState::Playing:
                    y += tr.buf[pos_];                   // read before write: the player hears the dry path
                    if (pos_ < tr.tailEnd) {             // tail of the first recording, g: 1 -> 0 over T = tailEnd
                        tr.buf[pos_] += x * float(tr.tailEnd - pos_) / float(tr.tailEnd);
                        if (pos_ + 1 == tr.tailEnd) tr.tailEnd = 0;
                    }
                    break;
                default:
                    break;
                }
            }
            if (L_ && ++pos_ == L_) pos_ = 0;
            out[i] = y;
        }
    }

    void pressRec(int t) {
        if (!valid(t)) return;
        Track& tr = tracks_[t];
        if (tr.state == TrackState::Empty && L_ == 0 && recTrack_ < 0) startFirst(t);
        else if (tr.state == TrackState::Recording) closeFirst();
    }

    void undo(int t) {
        if (!valid(t)) return;
        if (tracks_[t].state == TrackState::Recording) { tracks_[t].state = TrackState::Empty; recTrack_ = -1; }
    }

    void setTailMs(float ms) { tailMs_ = std::max(0.0f, std::min(ms, kMaxLoopSeconds * 1000.0f)); }
    void setTriggerLatencyMs(float ms) { trigMs_ = std::max(0.0f, std::min(ms, kHistoryMs)); }
    void setRecToDub(bool on) { recToDub_ = on; }   // stored only; Rec->Dub behaviour arrives with overdubs

    TrackState state(int t) const { return valid(t) ? tracks_[t].state : TrackState::Empty; }
    NextAction nextAction(int t) const {
        switch (state(t)) {
        case TrackState::Empty:     return NextAction::Rec;
        case TrackState::Recording: return NextAction::Play;
        case TrackState::Playing:   return NextAction::Dub;
        default:                    return NextAction::Play;
        }
    }
    uint32_t loopLength() const { return L_; }
    uint32_t position() const { return pos_; }
    uint32_t recordedSamples() const { return recTrack_ >= 0 ? rec_ - recD_ : 0; }   // what L would be if closed now
    double sampleRate() const { return sr_; }
    bool enabled() const { return enabled_; }

private:
    struct Track {
        float* buf = nullptr;
        TrackState state = TrackState::Empty;
        uint32_t tailEnd = 0;   // > 0: the input still blends into [pos, tailEnd) on the pass after the close
    };

    bool valid(int t) const { return enabled_ && t >= 0 && t < kTracks; }
    float fadeInGain(uint32_t i) const { return i < fadeIn_ ? float(i) / float(fadeIn_) : 1.0f; }

    // Back-date by D: the last D inputs from the history ring become buf[0..D).
    void startFirst(int t) {
        float* buf = tracks_[t].buf;
        recD_ = msToSamples(trigMs_, sr_);               // <= histLen_ (trigMs_ <= kHistoryMs)
        for (uint32_t i = 0; i < recD_; ++i)
            buf[i] = hist_[(histW_ + histLen_ - recD_ + i) % histLen_] * fadeInGain(i);
        rec_ = recD_;
        recTrack_ = t;
        tracks_[t].state = TrackState::Recording;
    }

    // The close is back-dated by the same D, so L = r - D and the D samples past it are tail.
    void closeFirst() {
        Track& tr = tracks_[recTrack_];
        const uint32_t L = rec_ - recD_;
        recTrack_ = -1;
        if (L < minLoop_) { tr.state = TrackState::Empty; return; }   // an accidental double press
        const uint32_t T = std::min(msToSamples(tailMs_, sr_), L / 2);
        for (uint32_t i = 0; i < std::min(recD_, T); ++i)             // fold the tail recorded so far
            tr.buf[i] += tr.buf[L + i] * float(T - i) / float(T);
        tr.tailEnd = recD_ < T ? T : 0;                               // the rest goes straight in while playing
        L_ = L;
        pos_ = recD_ % L;                                             // D <= histLen_ == minLoop_ <= L
        tr.state = TrackState::Playing;
    }

    bool fail() { release(); return false; }
    void release() {
        for (Track& tr : tracks_) { std::free(tr.buf); tr.buf = nullptr; }
        std::free(hist_); hist_ = nullptr;
        pool_.reserve(0);                                // frees the pool, reserves nothing
        enabled_ = false;
        reset();
    }

    Track tracks_[kTracks];
    BlockPool pool_;                 // overdub layer deltas for undo/redo (only reserved so far)
    float* hist_ = nullptr;          // input history ring, for back-dating presses
    uint32_t histLen_ = 0, histW_ = 0;
    double sr_ = 48000.0;
    uint32_t maxRec_ = 0, minLoop_ = 0, fadeIn_ = 0;
    uint32_t L_ = 0, pos_ = 0;       // shared loop length (0 = unset) and playhead
    uint32_t rec_ = 0, recD_ = 0;    // first recording: write index and its back-date D
    int recTrack_ = -1;
    float tailMs_ = 50.0f, trigMs_ = 3.0f;
    bool recToDub_ = false, enabled_ = false;
};

} // namespace ouro
