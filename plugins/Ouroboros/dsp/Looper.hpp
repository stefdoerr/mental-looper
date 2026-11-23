#pragma once
#include "BlockPool.hpp"
#include "Constants.hpp"
#include "Delta.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <new>

namespace ouro {

enum class TrackState : uint8_t { Empty, Recording, Playing, Overdubbing };
enum class NextAction : uint8_t { Rec, Play, Dub };
enum class Transport : uint8_t { Running, Stopping, Stopped };
enum class StopMode : uint8_t { Instant, EndOfLoop, Fade };

// A gain that moves linearly to a new target over n samples, from wherever it is.
struct Ramp {
    float v = 0.0f, to = 0.0f, step = 0.0f;
    uint32_t left = 0;
    float next(float target, uint32_t n) {
        if (target != to) { to = target; left = n; step = n ? (to - v) / float(n) : 0.0f; }
        v = left && --left ? v + step : to;
        return v;
    }
    void snap(float target) { v = to = target; left = 0; }
};

// The loop gain's schedule: after wait samples it moves from g0 over len samples, linearly up to 1 or down to 0
// along g0 * (1 - t)^3, t = done / len.
struct LoopGain {
    float v = 1.0f, g0 = 1.0f;
    uint32_t wait = 0, len = 0, done = 0;
    bool up = true;
    void start(bool toOne, uint32_t w, uint32_t n) { g0 = v; up = toOne; wait = w; len = n; done = 0; }
    bool ended() const { return !wait && done == len; }
    float next() {
        if (wait) --wait;
        else if (done < len) {
            const float t = float(++done) / float(len), u = 1.0f - t;
            v = up ? 1.0f - (1.0f - g0) * u : g0 * u * u * u;   // exactly 1 or 0 at the end
        }
        return v;
    }
};

// The output safety net: unchanged below the knee, then bends smoothly towards +-1.
inline float softclip(float y) {
    const float k = kSoftClipKnee, a = std::fabs(y);
    return a < k ? y : std::copysign(k + (1.0f - k) * std::tanh((a - k) / (1.0f - k)), y);
}

// The looper engine. init() is the only allocating call (non-RT). process() and the
// controls are RT-safe and single-threaded: controls are called between process() calls
// and take effect at the start of the next one.
class Looper {
public:
    Looper() = default;
    Looper(const Looper&) = delete;
    Looper& operator=(const Looper&) = delete;
    ~Looper() { release(); }

    // False => disabled: process() outputs only the dry path, and the actions (rec, undo, redo, clear) are ignored.
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

    // Every track empty, L unset, all undo/redo gone. Settings (on, volume, dry, bypass, ...) stay.
    void reset() {
        for (Track& tr : tracks_) {                      // hand every delta back to the pool
            wipe(tr);
            tr.state = TrackState::Empty;
            tr.gain.snap(0.0f);
            tr.len = tr.pos = 0;
        }
        L_ = pos_ = rec_ = recD_ = 0;
        recTrack_ = armed_ = -1;
        tp_ = Transport::Running;
        restartQueued_ = false;
        lg_ = LoopGain{};
    }

    void process(const float* in, float* out, uint32_t n) {
        const float dryTo = dryThru_ ? 1.0f : 0.0f;
        if (!enabled_) { for (uint32_t i = 0; i < n; ++i) out[i] = dry_.next(dryTo, ramp_) * in[i]; return; }
        for (uint32_t i = 0; i < n; ++i) {
            const float x = in[i];                       // read first: in and out may alias
            if (armed_ >= 0 && std::fabs(x) >= thr_) {   // the arm fires, back-dated by the pre-roll; x goes in below
                const int t = armed_;
                const uint32_t P = msToSamples(kPreRollMs, sr_);
                armed_ = -1;
                if (L_) startDub(tracks_[t], P, false, true, true, true); else startFirst(t, P);
            }
            if (recTrack_ >= 0 && rec_ == maxRec_) closeFirst(recToDub_);   // max length: close as if B were pressed
            // The last track went empty and its ramp ended: L resets. Stopped, the gains don't move: empty is enough.
            // An arm stays: it then starts the first loop.
            if (L_ && (tp_ == Transport::Stopped ? allEmpty() : idle())) { const int a = armed_; reset(); armed_ = a; }
            const uint32_t h = histW_;
            hist_[h] = x;
            if (++histW_ == histLen_) histW_ = 0;
            float y = dry_.next(dryTo, ramp_) * x, used = 0.0f;
            if (recTrack_ >= 0) {                        // first recording: overwrite (reused pages hold old audio)
                tracks_[recTrack_].buf[rec_] = x * (used = fadeInGain(rec_));   // closeFirst() rewrites the last D
                ++rec_;
            } else if (L_ && tp_ != Transport::Stopped) {   // stopped: only the dry path, nothing moves
                float wet = 0.0f;
                for (Track& tr : tracks_) wet += play(tr);   // read before write: the player hears the dry path
                y += lg_.next() * wet;
                for (Track& tr : tracks_) used += writeOut(tr, x);   // tails and fades first: the one track
                for (Track& tr : tracks_) if (writing(tr)) used += writeIn(tr, x, used);   // recording takes the rest
                if (++pos_ == L_) pos_ = 0;
                for (Track& tr : tracks_) if (tr.len && ++tr.pos == tr.len) tr.pos = 0;   // every playhead in step
                if (tp_ == Transport::Stopping && lg_.ended() && !overlap()) park();   // tails and fades done too
            }
            histUsed_[h] = used;
            out[i] = softclip(y);
        }
    }

    // Only one track records at a time: a press while another track records or overdubs first closes that one
    // (to Playing, never Rec->Dub), then does this track's own action. Not running, it only restarts.
    // Auto-record: an empty track arms instead (one at a time). A press cancels the arm; on another track it then acts.
    void pressRec(int t) {
        if (!valid(t)) return;
        if (tp_ != Transport::Running) { restart(); return; }
        if (armed_ >= 0) {
            const bool same = armed_ == t;
            armed_ = -1;
            if (same) return;
        }
        bool cont = false, offset = true;                // cont: the new writer complements the closed one's tail or
        for (int u = 0; u < kTracks; ++u)                // fade, at that one's offset (the first recording has none)
            if (u != t && writing(tracks_[u])) {
                const bool first = recTrack_ == u;
                cont = close(u, false);
                offset = !cont || (!first && tracks_[u].fade.offset);
            }
        Track& tr = tracks_[t];
        if (tr.state == TrackState::Empty && autoRec_) { armed_ = t; return; }
        const uint32_t D = msToSamples(trigMs_, sr_);
        switch (tr.state) {
        case TrackState::Empty:   if (L_) startDub(tr, D, cont, true, offset); else startFirst(t, D); break;
        case TrackState::Playing: startDub(tr, D, cont, false, offset); break;
        default:                  close(t, recToDub_); break;
        }
    }

    void undo(int t) {
        if (!valid(t)) return;
        if (armed_ >= 0) { armed_ = -1; return; }       // a tap on C cancels the arm, and only that
        Track& tr = tracks_[t];
        if (tr.state == TrackState::Recording) {         // abort: the track is empty again
            tr.state = TrackState::Empty;
            if (recTrack_ == t) recTrack_ = -1;
            return;
        }
        if (tr.state == TrackState::Overdubbing) {
            endDub(tr, true);                            // cancel: subtracted, not kept for redo
        } else if (tr.undoTop > 0) {
            if (tr.nOps >= kMaxPendingOps) return;       // layer undo/redo past the cap is ignored
            const uint32_t i = --tr.undoTop;
            // ponytail: undone during its 5 ms punch-out fade, the layer keeps a hard cut where the fade stopped, which
            // a redo brings back; let the fade finish into the delta if that is heard
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
        if (tr.baseUndone) {
            tr.baseUndone = false;
            tr.state = TrackState::Playing;
            if (armed_ == t) armed_ = -1;                // an arm is for an empty track only
        } else if (tr.undoTop < tr.redoTop && tr.nOps < kMaxPendingOps) {
            addOp(tr, tr.layers[tr.undoTop].d, 1.0f, true, int32_t(tr.undoTop));
            ++tr.undoTop;
        }
    }

    // Drops the track's layers and redo; it goes Empty and ramps out. The buffer stays for the next zero op.
    void clearTrack(int t) {
        if (!valid(t)) return;
        Track& tr = tracks_[t];
        if (recTrack_ == t) recTrack_ = -1;
        if (armed_ == t) armed_ = -1;
        dropLayers(tr);                                  // an op still reading a layer keeps it: the ramp-out is smooth
        tr.state = TrackState::Empty;
    }
    void clearAll() { for (int t = 0; t < kTracks; ++t) clearTrack(t); }

    // Closes every writer and cancels an arm, then fades the loops out. Stopping never changes the loop: the engine
    // parks at the top once the fade, every tail and every punch-out fade have ended. Instant: kRampMs now. EndOfLoop:
    // kRampMs ending at the top. A rest of kFadeInMs..kRampMs fades at once over the rest, still ending at the top; a
    // shorter rest fades over kFadeInMs, and the park then jumps back to the top. Fade: fadeMs (kRampMs..10 s) now,
    // cubic. A second stop cancels a queued restart and, while the loops still sound, cuts to Instant from the current
    // gain. A stop that closes the first recording parks silently in every mode (R7): Instant from loop gain 0.
    void stop(StopMode mode, float fadeMs = 3000.0f) {
        if (!enabled_ || tp_ == Transport::Stopped) return;
        if (tp_ == Transport::Stopping) {
            restartQueued_ = false;
            if (lg_.v > 0.0f) { stopMode_ = StopMode::Instant; lg_.start(false, 0, ramp_); }
            return;
        }
        armed_ = -1;
        const bool first = recTrack_ >= 0;
        for (int u = 0; u < kTracks; ++u) if (writing(tracks_[u])) close(u, false);
        if (!L_ || allEmpty()) return;                   // nothing to stop
        tp_ = Transport::Stopping;
        stopMode_ = first ? StopMode::Instant : mode;
        restartQueued_ = false;
        if (first) { lg_.v = 0.0f; lg_.start(false, 0, ramp_); return; }
        uint32_t wait = 0, len = ramp_;
        if (mode == StopMode::EndOfLoop) {
            // ponytail: started while the loop gain still rises (within kFadeInMs of a restart or kRampMs of a cancel;
            // automation or MIDI only), the wait holds the partial gain; let the rise finish during the wait if heard
            const uint32_t r = L_ - pos_;
            if (r > ramp_) wait = r - ramp_; else len = std::max(r, fadeIn_);
        } else if (mode == StopMode::Fade) {             // clamped in ms first: msToSamples(NaN or < 0) is undefined
            len = msToSamples(std::min(10000.0f, std::max(kRampMs, fadeMs)), sr_);
        }
        lg_.start(false, wait, len);
    }
    // Stopped: plays again from the top. The track gains did not move while stopped: they snap, and the loops fade in.
    // Stopping: a pending End of loop or Fade stop is cancelled (the loops ramp back up); otherwise the restart waits
    // for the park.
    void restart() {
        if (tp_ == Transport::Stopping) {
            if (stopMode_ != StopMode::Instant && !lg_.ended()) { tp_ = Transport::Running; lg_.start(true, 0, ramp_); }
            else restartQueued_ = true;
            return;
        }
        if (tp_ != Transport::Stopped) return;
        tp_ = Transport::Running;
        for (Track& tr : tracks_) tr.gain.snap(target(tr));
        lg_.v = 0.0f;
        lg_.start(true, 0, fadeIn_);
    }

    // Playback gains: settings, stored even while disabled; every gain ramps to its target over kRampMs.
    void setOn(int t, bool on) { if (inRange(t)) tracks_[t].on = on; }
    void toggleOn(int t) { if (inRange(t)) tracks_[t].on = !tracks_[t].on; }
    bool isOn(int t) const { return inRange(t) && tracks_[t].on; }
    void setVolumeDb(int t, float db) {
        if (!inRange(t)) return;
        tracks_[t].volDb = std::max(-60.0f, std::min(db, 6.0f));   // NaN -> -60
        tracks_[t].vol = std::pow(10.0f, tracks_[t].volDb / 20.0f);
    }
    float volumeDb(int t) const { return inRange(t) ? tracks_[t].volDb : 0.0f; }
    void setDryThru(bool on) { dryThru_ = on; }
    void setBypass(bool on) { bypass_ = on; }            // mutes the loops (they keep running); dry is unchanged
    void snapGains() {                                   // jump every gain to its target (first run after load)
        for (Track& tr : tracks_) tr.gain.snap(target(tr));
        dry_.snap(dryThru_ ? 1.0f : 0.0f);
    }

    void setTailMs(float ms) { tailMs_ = std::max(0.0f, std::min(ms, kMaxLoopSeconds * 1000.0f)); }
    void setTriggerLatencyMs(float ms) { trigMs_ = std::max(0.0f, std::min(ms, kHistoryMs)); }
    void setRecToDub(bool on) { recToDub_ = on; }
    void setAdvanceMs(float ms) { advMs_ = std::max(0.0f, std::min(ms, 30.0f)); }   // earlier only (R7)
    void setAutoRec(bool on) { autoRec_ = on; if (!on) armed_ = -1; }   // off: nothing starts by itself
    void setThresholdDb(float db) {                      // NaN -> -60
        thr_ = std::pow(10.0f, std::max(-60.0f, std::min(db, -10.0f)) / 20.0f);
    }

    // Not clamped here (the plugin's parameter range does that); the pool clamps to what it reserved.
    // Lowering it below the current use only takes effect at the next allocation.
    void setBudgetMb(float mb) { pool_.setBudgetBytes(mb > 0.0f ? size_t(double(mb) * 1048576.0) : 0); }   // NaN -> 0
    size_t undoBytes() const { return size_t(pool_.usedBlocks()) * BlockPool::kBlock * sizeof(float); }

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
    // A whole number of loops; 0 once the track is empty with no base kept for redo.
    uint32_t trackLength(int t) const {
        return valid(t) && (tracks_[t].state != TrackState::Empty || tracks_[t].baseUndone) ? tracks_[t].len : 0;
    }
    // Samples since the track's beginning, the pass start where it started: buffers are laid out by base phase.
    uint32_t trackPosition(int t) const { return trackLength(t) ? tracks_[t].pos : 0; }
    Transport transport() const { return tp_; }
    int armedTrack() const { return armed_; }            // -1: none
    // What L would be if closed now. An arm's start can be back-dated less than the close: 0 until rec_ passes recD_.
    uint32_t recordedSamples() const { return recTrack_ >= 0 && rec_ > recD_ ? rec_ - recD_ : 0; }
    double sampleRate() const { return sr_; }
    bool enabled() const { return enabled_; }

private:
    struct Layer { Delta d; uint32_t seq = 0; uint8_t refs = 0; };   // refs: pending ops reading d
    struct Op {                          // pending undo/redo: adds sign*d at the playhead over the len positions it owes
        Delta d;                         // a copy of the handle: the blocks are shared with the layer
        DeltaCursor c;
        float sign = 0.0f;
        uint32_t at = 0;                 // the next position it owes: it applies only when the playhead is there
        uint32_t done = 0, len = 0;      // positions applied; ramps for ramp_, ends at len (at most one pass)
        int32_t layer = -1;              // its layer while d is in a stack, else -1
        bool owns = false, ramp = false; // owns: releases d when it ends
        bool zero = false;               // a zero op: sets buf = 0 instead (a recording onto the loop)
    };
    struct Writer {                      // writes the overdub (or a recording onto the loop), or its punch-out fade
        int32_t layer = -1;              // index in layers; -1 = no delta (a recording onto the loop: the base)
        uint32_t w = 0, n = 0;           // write position; samples written (delta offset = n mod L)
        uint32_t left = 0;               // punch-out fade samples to go (0 = idle)
        float env = 0.0f, peak = 0.0f;   // last envelope; peak |v| written
        DeltaCursor c;
        bool cont = false, lost = false; // cont: envelope = 1 - what others write (Rec->Dub, handover); lost: pool full
        bool offset = true;              // written at the Offset behind the playhead (see startDub)
    };
    struct Track {
        float* buf = nullptr;            // the base, plus every layer still in the undo stack
        Layer* layers = nullptr;         // [0, undoTop) undo stack, [undoTop, redoTop) redo stack, top at undoTop
        uint32_t undoTop = 0, redoTop = 0;
        // In start order, a park's split parts first. The spare slot lets a cancel past the cap go through. x2: a
        // park can split every op (only ops added since the last restart can wrap; one is added only at nOps <= cap).
        Op ops[2 * (kMaxPendingOps + 1)];
        uint32_t nOps = 0;
        Writer dub, fade;
        TrackState state = TrackState::Empty;
        // The track's cycle (a whole number of loops, 0 = none) and playhead in buf. It outlives a clear or an abort
        // until the next recording or reset(): the ramp-out and the pending ops still read it.
        uint32_t len = 0, pos = 0;
        uint32_t tailEnd = 0;            // > 0: the input still blends into [pos, tailEnd) on the pass after the close
        Ramp gain;                       // output gain, towards target()
        float volDb = 0.0f, vol = 1.0f;  // volume in dB and linear
        bool on = true;
        bool baseUndone = false;         // the base is on top of the redo stack
    };

    static bool inRange(int t) { return t >= 0 && t < kTracks; }
    bool valid(int t) const { return enabled_ && inRange(t); }
    float fadeInGain(uint32_t i) const { return i < fadeIn_ ? float(i) / float(fadeIn_) : 1.0f; }
    static bool writing(const Track& tr) { return tr.state == TrackState::Recording || tr.state == TrackState::Overdubbing; }
    float target(const Track& tr) const { return tr.state != TrackState::Empty && tr.on && !bypass_ ? tr.vol : 0.0f; }
    bool idle() const {                                  // every track empty and ramped out
        for (const Track& tr : tracks_) if (tr.state != TrackState::Empty || tr.gain.v != 0.0f) return false;
        return true;
    }
    bool allEmpty() const {
        for (const Track& tr : tracks_) if (tr.state != TrackState::Empty) return false;
        return true;
    }
    bool overlap() const {                               // some track still takes a share of the input
        for (const Track& tr : tracks_) if (tr.pos < tr.tailEnd || tr.fade.left) return true;
        return false;
    }

    // Back-date by back (<= histLen_): the last back inputs from the history ring become buf[0..back). The close is
    // back-dated by the trigger latency recD_ whatever back was (a press: the same; an arm: the pre-roll).
    void startFirst(int t, uint32_t back) {
        float* buf = tracks_[t].buf;
        for (uint32_t i = 0; i < back; ++i)
            buf[i] = hist_[(histW_ + histLen_ - back + i) % histLen_] * fadeInGain(i);
        rec_ = back;
        recD_ = msToSamples(trigMs_, sr_);               // <= histLen_ (trigMs_ <= kHistoryMs)
        recTrack_ = t;
        tracks_[t].state = TrackState::Recording;
    }

    // Ends u's recording or overdub as its B press would. True if u still takes a share of the input after it
    // (a tail or punch-out fade): a track starting now takes the complement.
    bool close(int u, bool toDub) {
        Track& tr = tracks_[u];
        if (recTrack_ == u) return closeFirst(toDub);
        const bool base = tr.state == TrackState::Recording;   // a recording onto the loop
        endDub(tr, false);
        if (base && toDub) startDub(tr, 0, true, false, tr.fade.offset);   // Rec->Dub: the fade input splits g / 1-g
        return tr.fade.left > 0;
    }

    // The close is back-dated by the trigger latency D, so L = r - D and the D samples past it are tail. True if it has
    // a tail.
    bool closeFirst(bool toDub) {
        Track& tr = tracks_[recTrack_];
        const uint32_t L = recordedSamples();            // rec_ - recD_, never below 0
        recTrack_ = -1;
        if (L < minLoop_) { tr.state = TrackState::Empty; return false; }   // an accidental double press
        const uint32_t T = std::min(std::max(msToSamples(tailMs_, sr_), fadeIn_), L / 2);   // tail 0: a 3 ms crossfade
        for (uint32_t i = 0; i < std::min(recD_, T); ++i)             // fold the tail recorded so far
            tr.buf[i] += tr.buf[L + i] * float(T - i) / float(T);
        for (uint32_t i = 0; i < recD_; ++i)                          // the share those inputs went to the base
            histUsed_[(histW_ + histLen_ - recD_ + i) % histLen_] = i < T ? float(T - i) / float(T) : 0.0f;
        tr.tailEnd = recD_ < T ? T : 0;                               // the rest goes straight in while playing
        L_ = L;
        pos_ = recD_ % L;                                             // D <= histLen_ == minLoop_ <= L
        tr.len = L;
        tr.pos = pos_;
        tr.state = TrackState::Playing;
        tr.gain.snap(target(tr));                                     // no track ramp: the loops fade in instead
        // Playback starts D past the recording's own fade-in: the loops fade in over the part it skips, the restart's
        // kFadeInMs once D reaches it (R10). D = 0 skips nothing.
        if (recD_) { lg_.v = 0.0f; lg_.start(true, 0, std::min(recD_, fadeIn_)); }
        if (toDub) startDub(tr, recD_, true, false, false);           // Rec->Dub: the tail input splits g / 1-g
        return T > 0;
    }

    // Punch-in, back-dated by D: the last D inputs go to the D positions before the write position.
    // cont (Rec->Dub, handover): the layer takes exactly the complement of what the others write. Otherwise it
    // fades in, capped by the complement of a tail or punch-out fade still writing, so it never steps where it starts.
    // base: a recording onto the loop (L set, track Empty). It becomes the track's base, with no delta. The reused
    // buffer reads as silence: the positions behind the playhead are zeroed now, the rest by a zero op ahead of it.
    // offset: written at the Offset. A cont writer takes the offset of the one it complements, so a one-press chain
    // stays aligned: none after the first recording (Rec->Dub, handover before L), which has no offset.
    // trim: an arm's pre-roll. Its start is apart from the press that closed the last writer, so it always takes only
    // the history's remaining share: it never re-records input that writer took, whether or not its tail still runs.
    // ponytail: the zero op does not ramp; a re-record within kRampMs of a clear or base undo cuts that ramp-out
    // ponytail: a cancel or abort leaves its shares in the history; an arm firing within the pre-roll after one trims
    // that part of its pre-roll (not reachable by foot); take the shares back out on a cancel if that is hit
    void startDub(Track& tr, uint32_t D, bool cont, bool base = false, bool offset = true, bool trim = false) {
        const uint32_t back = D + (offset ? msToSamples(advMs_, sr_) : 0);   // the offset writes behind the playhead
        if (base) { wipe(tr); tr.len = L_; tr.pos = pos_; }   // a stale base or its pending ops never come back
        else clearRedo(tr);
        const uint32_t len = tr.len, p = tr.pos;
        Writer& dub = tr.dub;
        dub = Writer{};
        dub.w = (p + 2 * len - back) % len;
        dub.cont = cont;
        dub.offset = offset;
        if (base) {
            const uint32_t z = std::min(back, len);
            for (uint32_t i = 1; i <= z; ++i) tr.buf[(p + len - i) % len] = 0.0f;
            if (z < len) { Op& op = tr.ops[tr.nOps++]; op = Op{}; op.zero = true; op.at = p; op.len = len - z; }
        } else {
            dub.layer = int32_t(tr.undoTop);
            Layer& ly = tr.layers[tr.undoTop];
            ly.d.begin(dub.w);
            ly.seq = ++seq_;
            ly.refs = 0;
        }
        tr.state = base ? TrackState::Recording : TrackState::Overdubbing;   // first: evict() counts the new slot
        const bool split = trim || overlap();            // a press: else the history's shares may be stale (a cancel)
        // ponytail: a press within D of a close whose tail or punch-out fade has already ended (D > 5 ms, or Tail <= D
        // on the first loop) back-dates inputs the closed writer already took (split is false): they double. Not
        // reachable by foot at the 3 ms default (MIDI/automation only); fix by trimming presses as well, once a cancel
        // takes its shares back out of the history. An arm always trims.
        for (uint32_t i = 0; i < D; ++i) {
            const uint32_t k = (histW_ + histLen_ - D + i) % histLen_;
            const float rest = std::max(0.0f, 1.0f - histUsed_[k]);
            const float e = cont ? rest : split ? std::min(fadeInGain(i), rest) : fadeInGain(i);
            write(tr, dub, hist_[k] * e);
            dub.env = e;
            histUsed_[k] += e;
        }
    }

    // Punch-out (a layer, then a 5 ms fade that is not back-dated), or cancel/silent (subtracted, no layer).
    // A recording onto the loop (no delta) just gets the fade: it is the base.
    void endDub(Track& tr, bool cancel) {
        Writer& dub = tr.dub;
        tr.state = TrackState::Playing;
        if (dub.layer >= 0) {
            Delta& d = tr.layers[dub.layer].d;
            if (dub.lost) {                              // the pool was full: the audio stays, without undo
                d.release(pool_);
                dub.layer = -1;
            } else if (d.length() == 0) {
                return;                                  // nothing written (D = 0, both presses in one gap)
            } else if (cancel ? tr.nOps <= kMaxPendingOps : dub.peak < kSilenceFloor && tr.nOps < kMaxPendingOps) {
                addOp(tr, d, -1.0f, cancel, -1);         // the op owns the blocks now
                d.begin(0);
                return;                                  // all of it is subtracted: nothing to fade out
            } else {                                     // a layer (silent, or a cancel, only if no op slot is free)
                // ponytail: one spare slot: a 2nd cancel within one pass at the cap keeps its layer; more spares if hit
                tr.redoTop = ++tr.undoTop;
            }
        }
        // ponytail: a fade still running (PLAY, DUB, PLAY within 5 ms) is cut short; give it a second slot if heard
        tr.fade = dub;
        tr.fade.left = fadeOut_;
    }

    // Commits the pending ops that owe the playhead, then reads it through the track's gain.
    float play(Track& tr) {
        float adj = 0.0f;                                // holds back the ops' change during their ramp
        for (uint32_t k = 0; k < tr.nOps;) {
            Op& op = tr.ops[k];
            if (op.at != tr.pos) { ++k; continue; }      // a split part waiting for its stretch
            if (op.zero) {
                tr.buf[tr.pos] = 0.0f;
            } else {
                const uint32_t off = (tr.pos + tr.len - op.d.start()) % tr.len;
                if (off < op.d.length()) {
                    const float v = op.sign * op.d.read(pool_, op.c, off);
                    tr.buf[tr.pos] += v;
                    if (op.ramp && op.done < ramp_) adj -= v * (1.0f - float(op.done) / float(ramp_));
                }
            }
            if (++op.at == tr.len) op.at = 0;
            if (++op.done == op.len) endOp(tr, k); else ++k;
        }
        const float g = tr.gain.next(target(tr), ramp_);
        return g != 0.0f ? g * (tr.buf[tr.pos] + adj) : 0.0f;
    }

    // The outgoing shares of the input: the first recording's tail and the punch-out fade; returns their sum.
    float writeOut(Track& tr, float x) {
        float used = 0.0f;
        if (tr.pos < tr.tailEnd) {                       // tail of the first recording, g: 1 -> 0 over T = tailEnd
            tr.buf[tr.pos] += x * float(tr.tailEnd - tr.pos) / float(tr.tailEnd);
            used += float(tr.tailEnd - tr.pos) / float(tr.tailEnd);
            if (tr.pos + 1 == tr.tailEnd) tr.tailEnd = 0;
        }
        if (tr.fade.left) {                              // punch-out fade: env -> 0
            const float e = tr.fade.env * float(tr.fade.left) / float(fadeOut_);
            write(tr, tr.fade, x * e);
            used += e;
            --tr.fade.left;
        }
        return used;
    }

    // The recording or overdub takes what the others left (used): all of it (cont), else its fade-in capped by it.
    float writeIn(Track& tr, float x, float used) {
        const float rest = std::max(0.0f, 1.0f - used);
        const float e = tr.dub.cont ? rest : std::min(fadeInGain(tr.dub.n), rest);
        write(tr, tr.dub, x * e);
        tr.dub.env = e;
        return e;
    }

    void write(Track& tr, Writer& wr, float v) {
        tr.buf[wr.w] += v;
        if (wr.layer >= 0 && !wr.lost) store(tr, wr, v);
        wr.peak = std::max(wr.peak, std::fabs(v));
        if (++wr.w == tr.len) wr.w = 0;
        ++wr.n;
    }

    // The one place deltas grow: append on the first pass, accumulate after. If the pool is full the writer
    // stops storing; endDub() then releases the overdub's delta. A full pool first frees a redo entry, then the
    // globally-oldest undo layer.
    // ponytail: a punch-out fade that hits a full pool keeps a truncated delta (undo leaves <= 5 ms of fade)
    void store(Track& tr, Writer& wr, float v) {
        // evict() moves layer slots: re-resolve the delta by index after each one
        if (wr.n >= tr.len) tr.layers[wr.layer].d.accumulate(pool_, wr.c, wr.n % tr.len, v);
        else {
            while (!tr.layers[wr.layer].d.append(pool_, v))
                if (!evict()) { wr.lost = true; break; }
        }
    }

    // Frees one layer: a redo entry, else the oldest undo layer. A track offers the redo entry undone longest ago
    // (the far end of its redo stack, redone last), so its next redo stays the layer just undone; across tracks
    // the lowest seq goes. Layers read by a pending op or still written by a punch-out fade are skipped (when the
    // far end is still read, so is the rest of that redo stack: undone later, its ops end later). False if nothing
    // could be freed.
    // ponytail: O(tracks x layers) scan plus an array shift per call. Lowering the budget far below the use with
    // thousands of layers evicts them all in one block (multi-ms, an xrun); keep a per-track oldest index / a ring
    // of layers, or spread the evictions over blocks, if that is hit.
    bool evict() {
        Track* bt = nullptr;
        uint32_t bi = 0, best = UINT32_MAX;
        for (int pass = 0; pass < 2 && !bt; ++pass)      // pass 0: redo stacks, pass 1: undo stacks
            for (Track& tr : tracks_) {
                const uint32_t hi = pass ? tr.undoTop : tr.redoTop, lo = pass ? 0 : hi > tr.undoTop ? hi - 1 : hi;
                for (uint32_t i = lo; i < hi; ++i) {
                    const Layer& ly = tr.layers[i];
                    if (ly.refs || (tr.fade.left && tr.fade.layer == int32_t(i)) || ly.seq >= best) continue;
                    best = ly.seq; bt = &tr; bi = i;
                }
            }
        if (!bt) return false;
        Track& tr = *bt;
        tr.layers[bi].d.release(pool_);
        const uint32_t top = tr.redoTop + (tr.state == TrackState::Overdubbing ? 1 : 0);   // + the overdub's slot
        std::copy(tr.layers + bi + 1, tr.layers + top, tr.layers + bi);
        auto fix = [&](int32_t& l) { if (l > int32_t(bi) && l < int32_t(top)) --l; };
        fix(tr.dub.layer);
        fix(tr.fade.layer);
        for (uint32_t k = 0; k < tr.nOps; ++k) fix(tr.ops[k].layer);
        if (bi < tr.undoTop) --tr.undoTop;
        --tr.redoTop;
        return true;
    }

    void clearRedo(Track& tr) {
        for (uint32_t i = tr.undoTop; i < tr.redoTop; ++i) dropLayer(tr, i);
        tr.redoTop = tr.undoTop;
    }

    // Layer i left both stacks: release its blocks, or hand them to the newest op still reading them
    // (the last in array order ends last: every op owes at most one pass, and a park puts its split parts first).
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

    // Empties both stacks (and the overdub in progress) and stops the track's writers. The buffer stays.
    void dropLayers(Track& tr) {
        const uint32_t top = tr.redoTop + (tr.state == TrackState::Overdubbing ? 1 : 0);   // + the overdub's slot
        for (uint32_t i = 0; i < top; ++i) dropLayer(tr, i);
        tr.undoTop = tr.redoTop = 0;
        tr.dub = tr.fade = Writer{};
        tr.tailEnd = 0;
        tr.baseUndone = false;
    }

    // dropLayers, and the pending ops too (the buffer they would change is about to be zeroed or dropped).
    void wipe(Track& tr) {
        dropLayers(tr);                                  // first: it hands layers still read to their ops
        for (uint32_t k = 0; k < tr.nOps; ++k) if (tr.ops[k].owns) tr.ops[k].d.release(pool_);
        tr.nOps = 0;
    }

    // layer >= 0: d is that layer's delta; -1: the op owns d.
    void addOp(Track& tr, const Delta& d, float sign, bool ramp, int32_t layer) {
        Op& op = tr.ops[tr.nOps++];
        op = Op{};
        op.d = d;
        op.sign = sign;
        op.at = tr.pos;
        op.len = tr.len;
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

    // The stop's end: the playhead goes to the top. An op whose owed stretch wraps past the top splits there: a new
    // part A owes [0, wrap end) and the op keeps [at, L), with the delta ownership (it ends last). The A parts go
    // before every other op, so the last op in array order still ends last (see dropLayer).
    void park() {
        for (Track& tr : tracks_) {
            auto wraps = [&](const Op& o) { return o.at != 0 && o.at + (o.len - o.done) > tr.len; };
            uint32_t w = 0;
            for (uint32_t k = 0; k < tr.nOps; ++k) {
                tr.ops[k].ramp = false;                  // after a park the playhead only reads applied audio
                w += wraps(tr.ops[k]);
            }
            if (!w) continue;
            std::copy_backward(tr.ops, tr.ops + tr.nOps, tr.ops + tr.nOps + w);
            for (uint32_t k = w, a = 0; k < tr.nOps + w; ++k) {
                Op& o = tr.ops[k];
                if (!wraps(o)) continue;
                Op& A = tr.ops[a++];
                A = o;
                A.at = 0;
                A.len = o.at + (o.len - o.done) - tr.len;
                A.done = 0;
                A.owns = false;
                A.c = DeltaCursor{};
                if (A.layer >= 0) ++tr.layers[A.layer].refs;
                o.len = tr.len - o.at;
                o.done = 0;
            }
            tr.nOps += w;
        }
        pos_ = 0;
        for (Track& tr : tracks_) tr.pos = 0;            // every beginning is at the top
        tp_ = Transport::Stopped;
        if (restartQueued_) { restartQueued_ = false; restart(); }
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
    uint32_t L_ = 0, pos_ = 0;       // shared loop length (0 = unset) and base phase (position in the first loop)
    uint32_t rec_ = 0, recD_ = 0;    // first recording: write index and its back-date D
    uint32_t seq_ = 0;               // global layer sequence number (oldest first, across tracks)
    int recTrack_ = -1;              // the track making the first recording (L unset), else -1
    float tailMs_ = 50.0f, trigMs_ = 3.0f, advMs_ = 0.0f;
    Ramp dry_;
    LoopGain lg_;                    // multiplies the tracks' output only: the stop's fade-out, the restart's fade-in
    Transport tp_ = Transport::Running;
    StopMode stopMode_ = StopMode::Instant;
    bool restartQueued_ = false;     // B during a stop that cannot cancel: restart as soon as the engine parks
    int armed_ = -1;                 // the track auto-record starts at the threshold, else -1
    float thr_ = 0.01f;              // auto-record threshold, linear (-40 dB)
    bool autoRec_ = false;
    bool recToDub_ = false, enabled_ = false, dryThru_ = false, bypass_ = false;
};

} // namespace ouro
