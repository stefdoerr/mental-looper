#pragma once
#include "BlockPool.hpp"
#include "Constants.hpp"
#include "Delta.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

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
        fadeOut_ = msToSamples(kPunchOutFadeMs, sr_);
        ramp_ = msToSamples(kRampMs, sr_);
        histLen_ = msToSamples(kHistoryMs, sr_);
        for (Track& tr : tracks_)
            if (!(tr.buf = static_cast<float*>(std::calloc(size_t(bufLen), sizeof(float))))) return fail();
        if (!(hist_ = static_cast<float*>(std::calloc(histLen_, sizeof(float))))) return fail();
        if (!(histUsed_ = static_cast<float*>(std::calloc(histLen_, sizeof(float))))) return fail();
        size_t mb = size_t(kMaxBudgetMb);
        while (!pool_.reserve(mb << 20)) {
            if (mb == 16) return fail();
            mb = std::max<size_t>(mb / 2, 16);
        }
        pool_.setBudgetBytes(size_t(200) << 20);   // reserve() reset it to everything; 200 MB is the default
        // A stacked layer holds at least one block, so a track has at most one layer per block (+1 in progress).
        const size_t layers = (mb << 20) / (BlockPool::kBlock * sizeof(float)) + 1;
        for (Track& tr : tracks_)
            if (!(tr.layers = new (std::nothrow) Layer[layers]())) return fail();
        histW_ = 0;
        enabled_ = true;
        reset();
        return true;
    }

    void reset() {
        for (Track& tr : tracks_) {                      // hand every delta back to the pool
            for (uint32_t i = 0; i < tr.redoTop; ++i) tr.layers[i].d.release(pool_);
            if (tr.state == TrackState::Overdubbing) tr.layers[tr.undoTop].d.release(pool_);
            for (uint32_t k = 0; k < tr.nOps; ++k) if (tr.ops[k].owns) tr.ops[k].d.release(pool_);
            tr.undoTop = tr.redoTop = tr.nOps = 0;
            tr.dub = tr.fade = Writer{};
            tr.state = TrackState::Empty;
            tr.tailEnd = 0;
            tr.gain = 0.0f;
            tr.baseUndone = false;
        }
        L_ = pos_ = rec_ = recD_ = 0;
        recTrack_ = -1;
    }

    void process(const float* in, float* out, uint32_t n) {
        if (!enabled_) { std::memset(out, 0, n * sizeof(float)); return; }
        for (uint32_t i = 0; i < n; ++i) {
            const float x = in[i];                       // read first: in and out may alias
            if (recTrack_ >= 0 && rec_ == maxRec_) closeFirst();   // max length: close as if B were pressed
            const uint32_t h = histW_;
            hist_[h] = x;
            if (++histW_ == histLen_) histW_ = 0;
            float y = 0.0f, used = 0.0f;
            for (Track& tr : tracks_) {
                if (tr.state == TrackState::Recording) { // first recording: overwrite (reused pages hold old audio)
                    tr.buf[rec_] = x * fadeInGain(rec_);
                    ++rec_;
                    continue;
                }
                y += play(tr);                           // read before write: the player hears the dry path
                used += writeInput(tr, x);
            }
            histUsed_[h] = used;
            if (L_ && ++pos_ == L_) pos_ = 0;
            out[i] = y;
        }
    }

    void pressRec(int t) {
        if (!valid(t)) return;
        Track& tr = tracks_[t];
        switch (tr.state) {
        case TrackState::Empty:       if (L_ == 0 && recTrack_ < 0) startFirst(t); break;
        case TrackState::Recording:   closeFirst(); break;
        case TrackState::Playing:     startDub(tr, msToSamples(trigMs_, sr_), false); break;
        case TrackState::Overdubbing: endDub(tr, false); break;
        }
    }

    void undo(int t) {
        if (!valid(t)) return;
        Track& tr = tracks_[t];
        if (tr.state == TrackState::Recording) { tr.state = TrackState::Empty; recTrack_ = -1; return; }
        if (tr.nOps == kMaxPendingOps) return;           // presses past the cap are ignored
        if (tr.state == TrackState::Overdubbing) {
            endDub(tr, true);                            // cancel: subtracted, not kept for redo
        } else if (tr.undoTop > 0) {
            const uint32_t i = --tr.undoTop;
            if (tr.fade.left && tr.fade.layer == int32_t(i)) tr.fade.left = 0;   // stop writing into it
            addOp(tr, tr.layers[i].d, -1.0f, true, int32_t(i));
        } else if (tr.state == TrackState::Playing) {    // the base: the output ramps out, the buffer stays for redo
            tr.state = TrackState::Empty;
            tr.baseUndone = true;
        }
    }

    void redo(int t) {
        if (!valid(t)) return;
        Track& tr = tracks_[t];
        if (tr.nOps == kMaxPendingOps) return;
        if (tr.baseUndone) {
            tr.baseUndone = false;
            tr.state = TrackState::Playing;
        } else if (tr.undoTop < tr.redoTop) {
            addOp(tr, tr.layers[tr.undoTop].d, 1.0f, true, int32_t(tr.undoTop));
            ++tr.undoTop;
        }
    }

    void setTailMs(float ms) { tailMs_ = std::max(0.0f, std::min(ms, kMaxLoopSeconds * 1000.0f)); }
    void setTriggerLatencyMs(float ms) { trigMs_ = std::max(0.0f, std::min(ms, kHistoryMs)); }
    void setRecToDub(bool on) { recToDub_ = on; }
    void setAdvanceMs(float ms) { advMs_ = std::max(0.0f, std::min(ms, 30.0f)); }   // earlier only (R7)

    TrackState state(int t) const { return valid(t) ? tracks_[t].state : TrackState::Empty; }
    NextAction nextAction(int t) const {
        switch (state(t)) {
        case TrackState::Empty:     return NextAction::Rec;
        case TrackState::Recording: return recToDub_ ? NextAction::Dub : NextAction::Play;
        case TrackState::Playing:   return NextAction::Dub;
        default:                    return NextAction::Play;
        }
    }
    bool canUndo(int t) const { return state(t) != TrackState::Empty; }
    bool canRedo(int t) const { return valid(t) && (tracks_[t].baseUndone || tracks_[t].undoTop < tracks_[t].redoTop); }
    uint32_t undoDepth(int t) const { return valid(t) ? tracks_[t].undoTop : 0; }
    uint32_t pendingOps(int t) const { return valid(t) ? tracks_[t].nOps : 0; }
    uint32_t loopLength() const { return L_; }
    uint32_t position() const { return pos_; }
    uint32_t recordedSamples() const { return recTrack_ >= 0 ? rec_ - recD_ : 0; }   // what L would be if closed now
    double sampleRate() const { return sr_; }
    bool enabled() const { return enabled_; }

private:
    struct Layer { Delta d; uint32_t seq = 0; uint8_t refs = 0; };   // refs: pending ops reading d
    struct Op {                          // pending undo/redo: adds sign*d at the playhead over one pass
        Delta d;                         // a copy of the handle: the blocks are shared with the layer
        DeltaCursor c;
        float sign = 0.0f;
        uint32_t done = 0;               // samples since it started; ramps for ramp_, ends at L
        int32_t layer = -1;              // its layer while d is in a stack, else -1
        bool owns = false, ramp = false; // owns: releases d when it ends
    };
    struct Writer {                      // writes one layer: the overdub, or its punch-out fade
        int32_t layer = -1;              // index in layers; -1 = no delta
        uint32_t w = 0, n = 0;           // write position; samples written (delta offset = n mod L)
        uint32_t left = 0;               // punch-out fade samples to go (0 = idle)
        float env = 0.0f, peak = 0.0f;   // last envelope; peak |v| written
        DeltaCursor c;
        bool cont = false, lost = false; // cont: envelope = 1 - what others write (Rec->Dub); lost: pool was full
    };
    struct Track {
        float* buf = nullptr;
        Layer* layers = nullptr;         // [0, undoTop) undo stack, [undoTop, redoTop) redo stack, top at undoTop
        uint32_t undoTop = 0, redoTop = 0;
        Op ops[kMaxPendingOps];          // in start order
        uint32_t nOps = 0;
        Writer dub, fade;
        TrackState state = TrackState::Empty;
        uint32_t tailEnd = 0;            // > 0: the input still blends into [pos, tailEnd) on the pass after the close
        float gain = 0.0f;               // output ramp for base undo/redo
        bool baseUndone = false;         // the base is on top of the redo stack
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
        for (uint32_t i = 0; i < recD_; ++i)                          // the share those inputs went to the base
            histUsed_[(histW_ + histLen_ - recD_ + i) % histLen_] = i < T ? float(T - i) / float(T) : 0.0f;
        tr.tailEnd = recD_ < T ? T : 0;                               // the rest goes straight in while playing
        L_ = L;
        pos_ = recD_ % L;                                             // D <= histLen_ == minLoop_ <= L
        tr.state = TrackState::Playing;
        tr.gain = 1.0f;                                               // no ramp: the recording has its fade-in
        if (recToDub_) startDub(tr, recD_, true);                     // Rec->Dub: the tail input splits g / 1-g
    }

    // Punch-in, back-dated by D: the last D inputs go to the D positions before the write position.
    // cont (Rec->Dub): the layer takes exactly the complement of the tail. Otherwise it fades in, capped by
    // the complement of a tail or punch-out fade still writing, so it never steps where it starts.
    void startDub(Track& tr, uint32_t D, bool cont) {
        clearRedo(tr);
        Writer& dub = tr.dub;
        dub = Writer{};
        dub.layer = int32_t(tr.undoTop);
        dub.w = (pos_ + 2 * L_ - D - msToSamples(advMs_, sr_)) % L_;   // the offset writes behind the playhead
        Layer& ly = tr.layers[tr.undoTop];
        ly.d.begin(dub.w);
        ly.seq = ++seq_;
        ly.refs = 0;
        dub.cont = cont;
        const bool split = pos_ < tr.tailEnd || tr.fade.left;      // else the history's shares are stale
        for (uint32_t i = 0; i < D; ++i) {
            const uint32_t k = (histW_ + histLen_ - D + i) % histLen_;
            const float rest = std::max(0.0f, 1.0f - histUsed_[k]);
            const float e = cont ? rest : split ? std::min(fadeInGain(i), rest) : fadeInGain(i);
            write(tr, dub, hist_[k] * e);
            dub.env = e;
            histUsed_[k] += e;
        }
        tr.state = TrackState::Overdubbing;
    }

    // Punch-out (a layer, then a 5 ms fade that is not back-dated), or cancel/silent (subtracted, no layer).
    void endDub(Track& tr, bool cancel) {
        Writer& dub = tr.dub;
        Delta& d = tr.layers[dub.layer].d;
        tr.state = TrackState::Playing;
        if (dub.lost) {                                  // the pool was full: the audio stays, without undo
            d.release(pool_);
            dub.layer = -1;
        } else if (d.length() == 0) {
            return;                                      // nothing written (D = 0, both presses in one gap)
        } else if ((cancel || dub.peak < kSilenceFloor) && tr.nOps < kMaxPendingOps) {
            addOp(tr, d, -1.0f, cancel, -1);             // the op owns the blocks now
            d.begin(0);
            return;                                      // all of it is subtracted: nothing to fade out
        } else {                                         // a layer (silent only if no op slot is free)
            tr.redoTop = ++tr.undoTop;
        }
        // ponytail: a fade still running (PLAY, DUB, PLAY within 5 ms) is cut short; give it a second slot if heard
        tr.fade = dub;
        tr.fade.left = fadeOut_;
    }

    // Commits the pending ops at the playhead, then reads it.
    float play(Track& tr) {
        float adj = 0.0f;                                // holds back the ops' change during their ramp
        for (uint32_t k = 0; k < tr.nOps;) {
            Op& op = tr.ops[k];
            const uint32_t off = (pos_ + L_ - op.d.start()) % L_;
            if (off < op.d.length()) {
                const float v = op.sign * op.d.read(pool_, op.c, off);
                tr.buf[pos_] += v;
                if (op.ramp && op.done < ramp_) adj -= v * (1.0f - float(op.done) / float(ramp_));
            }
            if (++op.done == L_) endOp(tr, k); else ++k;
        }
        const float target = tr.state == TrackState::Empty ? 0.0f : 1.0f;
        const float step = 1.0f / float(ramp_);
        tr.gain = tr.gain < target ? std::min(target, tr.gain + step) : std::max(target, tr.gain - step);
        return tr.gain > 0.0f ? tr.gain * (tr.buf[pos_] + adj) : 0.0f;
    }

    // Adds the input to the base tail, the punch-out fade and the overdub; returns the envelopes' sum.
    float writeInput(Track& tr, float x) {
        float used = 0.0f;
        if (pos_ < tr.tailEnd) {                         // tail of the first recording, g: 1 -> 0 over T = tailEnd
            tr.buf[pos_] += x * float(tr.tailEnd - pos_) / float(tr.tailEnd);
            used += float(tr.tailEnd - pos_) / float(tr.tailEnd);
            if (pos_ + 1 == tr.tailEnd) tr.tailEnd = 0;
        }
        if (tr.fade.left) {                              // punch-out fade: env -> 0
            const float e = tr.fade.env * float(tr.fade.left) / float(fadeOut_);
            write(tr, tr.fade, x * e);
            used += e;
            --tr.fade.left;
        }
        if (tr.state == TrackState::Overdubbing) {
            const float rest = std::max(0.0f, 1.0f - used);
            const float e = tr.dub.cont ? rest : std::min(fadeInGain(tr.dub.n), rest);
            write(tr, tr.dub, x * e);
            tr.dub.env = e;
            used += e;
        }
        return used;
    }

    void write(Track& tr, Writer& wr, float v) {
        tr.buf[wr.w] += v;
        if (!wr.lost) store(tr.layers[wr.layer].d, wr, v);
        wr.peak = std::max(wr.peak, std::fabs(v));
        if (++wr.w == L_) wr.w = 0;
        ++wr.n;
    }

    // The one place deltas grow: append on the first pass, accumulate after. If the pool is full the writer
    // stops storing; endDub() then releases the overdub's delta.
    // ponytail: a punch-out fade that hits a full pool keeps a truncated delta (undo leaves <= 5 ms of fade)
    void store(Delta& d, Writer& wr, float v) {
        if (wr.n >= L_) d.accumulate(pool_, wr.c, wr.n % L_, v);
        else if (!d.append(pool_, v)) wr.lost = true;
    }

    void clearRedo(Track& tr) {
        for (uint32_t i = tr.undoTop; i < tr.redoTop; ++i) dropLayer(tr, i);
        tr.redoTop = tr.undoTop;
    }

    // Layer i left both stacks: release its blocks, or hand them to the newest op still reading them
    // (every op lasts one pass, so the newest ends last).
    void dropLayer(Track& tr, uint32_t i) {
        Layer& ly = tr.layers[i];
        if (ly.refs == 0) { ly.d.release(pool_); return; }
        Op* newest = nullptr;
        for (uint32_t k = 0; k < tr.nOps; ++k)
            if (tr.ops[k].layer == int32_t(i)) { tr.ops[k].layer = -1; newest = &tr.ops[k]; }
        newest->owns = true;
        ly.refs = 0;
        ly.d.begin(0);
    }

    // layer >= 0: d is that layer's delta; -1: the op owns d.
    void addOp(Track& tr, const Delta& d, float sign, bool ramp, int32_t layer) {
        Op& op = tr.ops[tr.nOps++];
        op.d = d;
        op.c = DeltaCursor{};
        op.sign = sign;
        op.done = 0;
        op.layer = layer;
        op.owns = layer < 0;
        op.ramp = ramp;
        if (layer >= 0) ++tr.layers[layer].refs;
    }

    void endOp(Track& tr, uint32_t k) {
        Op& op = tr.ops[k];
        if (op.owns) op.d.release(pool_);
        else if (op.layer >= 0) --tr.layers[op.layer].refs;
        std::copy(tr.ops + k + 1, tr.ops + tr.nOps, tr.ops + k);   // keep start order
        --tr.nOps;
    }

    bool fail() { release(); return false; }
    void release() {
        reset();                                         // returns the deltas while the pool still exists
        for (Track& tr : tracks_) {
            std::free(tr.buf); tr.buf = nullptr;
            delete[] tr.layers; tr.layers = nullptr;
        }
        std::free(hist_); hist_ = nullptr;
        std::free(histUsed_); histUsed_ = nullptr;
        pool_.reserve(0);                                // frees the pool, reserves nothing
        enabled_ = false;
    }

    Track tracks_[kTracks];
    BlockPool pool_;                 // overdub layer deltas for undo/redo
    float* hist_ = nullptr;          // input history ring, for back-dating presses
    float* histUsed_ = nullptr;      // per history input: the envelope share already written somewhere (splits)
    uint32_t histLen_ = 0, histW_ = 0;
    double sr_ = 48000.0;
    uint32_t maxRec_ = 0, minLoop_ = 0, fadeIn_ = 0, fadeOut_ = 0, ramp_ = 0;
    uint32_t L_ = 0, pos_ = 0;       // shared loop length (0 = unset) and playhead
    uint32_t rec_ = 0, recD_ = 0;    // first recording: write index and its back-date D
    uint32_t seq_ = 0;               // global layer sequence number (oldest first, across tracks)
    int recTrack_ = -1;
    float tailMs_ = 50.0f, trigMs_ = 3.0f, advMs_ = 0.0f;
    bool recToDub_ = false, enabled_ = false;
};

} // namespace ouro
