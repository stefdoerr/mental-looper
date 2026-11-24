#pragma once
#include "BlockPool.hpp"
#include "Constants.hpp"
#include "Delta.hpp"
#include "TrackBuf.hpp"
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
    // poolBytes: the pool for the tracks' audio and the undo layers; 0 = as much as was reserved before they shared
    // it (kTracks full-length tracks plus kMaxBudgetMb), halving on failure down to one full-length track plus 16 MB.
    // A size given (tests) is used as it is.
    bool init(double sampleRate, uint32_t /*maxBlock: the engine runs per sample, no block scratch*/, size_t poolBytes = 0) {
        release();
        // Positions are uint32_t: reject rates whose reservation would not fit (also NaN and absurdly low rates).
        const double bufLen = (double(kMaxLoopSeconds) + 1.0) * sampleRate;   // 5 min + 1 s slack (tail, history)
        if (!(sampleRate >= 1000.0) || bufLen > double(UINT32_MAX)) return false;
        sr_ = sampleRate;
        cap_ = uint32_t(bufLen);
        maxRec_ = msToSamples(kMaxLoopSeconds * 1000.0f, sr_);
        minLoop_ = msToSamples(kMinLoopMs, sr_);
        fadeIn_ = msToSamples(kFadeInMs, sr_);
        fadeOut_ = msToSamples(kPunchOutFadeMs, sr_);
        ramp_ = msToSamples(kRampMs, sr_);
        histLen_ = msToSamples(kHistoryMs, sr_);
        for (Track& tr : tracks_)
            if (!tr.buf.init(cap_)) return fail();
        if (!(hist_ = static_cast<float*>(std::calloc(histLen_, sizeof(float))))) return fail();
        if (!(histUsed_ = static_cast<float*>(std::calloc(histLen_, sizeof(float))))) return fail();
        const uint64_t track = uint64_t(cap_) * sizeof(float), least = track + (uint64_t(16) << 20);
        uint64_t bytes = poolBytes ? poolBytes : kTracks * track + (uint64_t(kMaxBudgetMb) << 20);
        while (bytes > SIZE_MAX || !pool_.reserve(size_t(bytes))) {
            if (poolBytes || bytes == least) return fail();
            bytes = std::max(bytes / 2, least);
        }
        pool_.setBudgetBytes(size_t(200) << 20);   // reserve() reset it to everything; 200 MB is the default
        // A stacked layer holds at least one block, so a track has at most one layer per block (+1 in progress).
        const size_t layers = size_t(bytes / (BlockPool::kBlock * sizeof(float))) + 1;
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
            tr.len = tr.pos = tr.phase = 0;
        }
        L_ = pos_ = rec_ = recD_ = 0;
        recTrack_ = armed_ = -1;
        pend_ = {};
        tp_ = Transport::Running;
        restartQueued_ = false;
        lg_ = LoopGain{};
        xf_ = 0;
    }

    void process(const float* in, float* out, uint32_t n) {
        const float dryTo = dryThru_ ? 1.0f : 0.0f;
        if (!enabled_) { for (uint32_t i = 0; i < n; ++i) out[i] = dry_.next(dryTo, ramp_) * in[i]; return; }
        for (uint32_t i = 0; i < n; ++i) {
            const float x = in[i];                       // read first: in and out may alias
            if (pend_.kind) {                            // the waiting action acts on the line (no grid now: at once)
                double ph;                               // Sync off: never waiting, so only this test per sample
                at_ = i;
                if (!gridPhase(ph) || ph < 1.0) {
                    const Pending p = pend_;
                    pend_ = {};
                    if (p.retrig) retrigger();           // MOD's start first: a press then acts on the new timeline
                    if (p.kind == Pending::Press) pressRec(p.t);
                    else if (p.kind == Pending::Arm) fireArm(p.t);
                    else if (p.kind == Pending::Restart) restart();
                }
                at_ = 0;
            }
            if (armed_ >= 0 && std::fabs(x) >= thr_) {   // the arm fires; x goes in below
                const int t = armed_;
                armed_ = -1;
                at_ = i; fireArm(t); at_ = 0;
            }
            // Max length, or memory full (the last write was refused, see below): close as if B were pressed, but at
            // once (with a grid, on the last line passed). A press waiting to close it goes; one waiting on another
            // track starts that track on the next line.
            if (recTrack_ >= 0 && (rec_ == maxRec_ || pool_.full())) {
                if (pend_.t == recTrack_) pend_ = {};
                at_ = i; closeFirst(recToDub_); at_ = 0;
            }
            // Length: the first recording closes on its Nth line, as a press waiting for it would. Sync off: flag tests.
            if (fixedBars_ && recTrack_ >= 0 && gValid_) {
                double ph;
                at_ = i;
                if (gridPhase(ph) && ph < 1.0 && std::lround((rec_ - ph) / spb_) >= long(fixedBars_)) {
                    if (pend_.t == recTrack_) pend_ = {};
                    closeFirst(recToDub_);
                }
                at_ = 0;
            }
            // A recording onto the loop at the most whole loops in 5 min, or a take whose write was refused (memory full,
            // see write()): close as if B were pressed.
            for (int u = 0; u < kTracks; ++u) {
                const Track& tr = tracks_[u];
                if (writing(tr) && (tr.dub.full || (tr.len == cap_ && tr.pos == maxRec_ / L_ * L_))) close(u, recToDub_);
            }
            // The last track went empty and its ramp ended: L resets. Stopped, the gains don't move: empty is enough.
            // An arm stays, and so does a track's waiting action: it then starts the first loop. MOD's retrigger goes.
            if (L_ && (tp_ == Transport::Stopped ? allEmpty() : idle())) {
                const int a = armed_; const Pending p = pend_;
                reset();
                armed_ = a; if (p.t >= 0) pend_ = {p.kind, p.t};
            }
            const uint32_t h = histW_;
            hist_[h] = x;
            if (++histW_ == histLen_) histW_ = 0;
            float y = dry_.next(dryTo, ramp_) * x, used = 0.0f;
            if (recTrack_ >= 0) {                        // first recording, into an empty buffer (see startFirst)
                // closeFirst() rewrites the last D. fadeInGain(rec_), over recFadeIn_ (0 after a lead-in). Refused
                // (memory full), x is not recorded: the next sample closes the loop at rec_.
                if (float* s = sample(tracks_[recTrack_], rec_)) {
                    *s = x * (used = rec_ < recFadeIn_ ? float(rec_) / float(fadeIn_) : 1.0f);
                    ++rec_;
                }
            } else if (L_ && tp_ != Transport::Stopped) {   // stopped: only the dry path, nothing moves
                float wet = 0.0f;
                for (Track& tr : tracks_) wet += play(tr);   // read before write: the player hears the dry path
                if (xf_) wet = crossfade(wet);           // a retrigger's 3 ms; else only this test
                y += lg_.next() * wet;
                for (Track& tr : tracks_) used += writeOut(tr, x);   // tails and fades first: the one track
                for (Track& tr : tracks_) if (writing(tr)) used += writeIn(tr, x, used);   // recording takes the rest
                if (++pos_ == L_) pos_ = 0;
                for (Track& tr : tracks_) if (tr.len && ++tr.pos == tr.len) tr.pos = 0;   // every playhead in step
                // Tails and fades done too. A queued restart then asks the grid at the next sample, its first.
                if (tp_ == Transport::Stopping && lg_.ended() && !overlap()) { at_ = i + 1; park(); at_ = 0; }
            }
            histUsed_[h] = used;
            out[i] = softclip(y);
        }
        if (gValid_) modPh_ = phaseIn(modPh_ + n, spb_);  // MOD's grid moves on: once per block, never with Sync off
    }

    // Only one track records at a time: a press while another track records or overdubs first closes that one
    // (to Playing, never Rec->Dub), then does this track's own action. Not running, it only restarts.
    // Auto-record: an empty track arms instead (one at a time). A press cancels the arm; on another track it then acts.
    // Sync, with a grid: a press that starts a recording (first or onto the loop) or closes the first one goes on a bar
    // line. Up to kSyncGraceMs after a line it acts now, back-dated to the line instead of by the trigger latency;
    // earlier, it waits for the next line. One action waits at a time, like the arm: a press cancels it, and on another
    // track then acts (a waiting restart, any press only cancels). An arm's start and a restart go on a line too (see
    // fireArm() and restart()). MOD's retrigger waiting for its line stays (see modStarted()).
    void pressRec(int t) {
        if (!valid(t)) return;
        if (pend_.kind && pend_.kind != Pending::Retrigger) {
            const bool same = pend_.t == t || pend_.kind == Pending::Restart;
            cancelWaiting();
            if (same) return;
        }
        if (tp_ != Transport::Running) { restart(); return; }
        if (armed_ >= 0) {
            const bool same = armed_ == t;
            armed_ = -1;
            if (same) return;
        }
        double ph;                                       // the free grid's first line (nothing recorded, MOD stopped)
        const bool snap = (recTrack_ >= 0 || (tracks_[t].state == TrackState::Empty && !autoRec_ && (L_ || gPlaying_)))
                          && gridPhase(ph);              // is the press: back-dated by the trigger latency, no snap
        const uint32_t since = snap ? uint32_t(ph) : 0;
        if (since > msToSamples(kSyncGraceMs, sr_)) { pend_ = {Pending::Press, t, pend_.retrig}; return; }
        bool cont = false, offset = true, trim = false;  // cont: the new writer complements the closed one's tail or
        for (int u = 0; u < kTracks; ++u)                // fade, at that one's offset (the first recording has none);
            if (u != t && writing(tracks_[u])) {         // trim: a first loop closed off this press's back-date (a
                const bool first = recTrack_ == u;       // tempo change, see closeFirst): it fades in under the rest
                cont = close(u, false);
                offset = !first && (!cont || tracks_[u].fade.offset);
                trim = first && !cont;
            }
        Track& tr = tracks_[t];
        if (tr.state == TrackState::Empty && autoRec_) { armed_ = t; return; }
        const uint32_t D = snap ? since : msToSamples(trigMs_, sr_);
        switch (tr.state) {
        case TrackState::Empty:   if (L_) startDub(tr, D, cont, true, offset, trim); else startFirst(t, D); break;
        case TrackState::Playing: startDub(tr, D, cont, false, offset); break;
        default:                  close(t, recToDub_); break;
        }
    }

    void undo(int t) {
        if (!valid(t)) return;
        if (armed_ >= 0) { armed_ = -1; return; }       // a tap on C cancels the arm, and only that; likewise the
        if (userWaiting()) { cancelWaiting(); return; } // user's wait, on any track (not MOD's)
        Track& tr = tracks_[t];
        if (tr.state == TrackState::Recording) {         // abort: the track is empty again. None of the take was heard
            tr.state = TrackState::Empty;                // (a recording onto the loop plays ahead of its writer): its
            tr.buf.clear(pool_);                         // blocks go back now
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
            if (armed_ == t) armed_ = -1;                // an arm is for an empty track only, and so is an action
            if (pend_.t == t) cancelWaiting();           // waiting for the line on it
        } else if (tr.undoTop < tr.redoTop && tr.nOps < kMaxPendingOps) {
            addOp(tr, tr.layers[tr.undoTop].d, 1.0f, true, int32_t(tr.undoTop));
            ++tr.undoTop;
        }
    }

    // Drops the track's layers and redo; it goes Empty and ramps out. The buffer and the pending ops stay.
    void clearTrack(int t) {
        if (!valid(t)) return;
        Track& tr = tracks_[t];
        if (recTrack_ == t) recTrack_ = -1;
        if (armed_ == t) armed_ = -1;
        if (pend_.t == t) cancelWaiting();
        dropLayers(tr);                                  // an op still reading a layer keeps it: the ramp-out is smooth
        if (tr.state == TrackState::Recording) tr.buf.clear(pool_);   // a take none of which was heard (see undo())
        tr.state = TrackState::Empty;
    }
    void clearAll() { for (int t = 0; t < kTracks; ++t) clearTrack(t); }

    // Closes every writer and cancels an arm or a waiting action, then fades the loops out (stopped: it only cancels a
    // waiting restart). Stopping never changes the loop: the engine parks (see park()) once the fade, every tail and
    // every punch-out fade have ended. Instant: kRampMs now.
    // EndOfLoop: kRampMs ending where the longest track's pass ends (measured after the closes). A rest of
    // kFadeInMs..kRampMs fades at once over the rest, still ending there; a shorter rest fades over kFadeInMs, and the
    // park then jumps back to the longest track's beginning. Fade: fadeMs (kRampMs..10 s) now,
    // cubic. A second stop cancels a queued restart and, while the loops still sound, cuts to Instant from the current
    // gain. A stop that closes the first recording parks silently in every mode (R7): Instant from loop gain 0.
    void stop(StopMode mode, float fadeMs = 3000.0f) {
        if (!enabled_) return;
        pend_ = {};
        if (tp_ == Transport::Stopped) return;
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
            uint32_t ml;
            const uint32_t mp = longest(ml), r = ml - mp;
            if (r > ramp_) wait = r - ramp_; else len = std::max(r, fadeIn_);
        } else if (mode == StopMode::Fade) {             // clamped in ms first: msToSamples(NaN or < 0) is undefined
            len = msToSamples(std::min(10000.0f, std::max(kRampMs, fadeMs)), sr_);
        }
        lg_.start(false, wait, len);
    }
    // Stopped: plays again from the top. The track gains did not move while stopped: they snap, and the loops fade in.
    // With a grid it goes on a line, as a press does: up to kSyncGraceMs after one it starts now, every playhead
    // advanced by the samples since, as if it had started on the line; earlier, it waits for the next line. MOD
    // stopped, the loop's own grid is at its top (the park put it there), so at once. Again while it waits: cancelled.
    // Stopping: a pending End of loop or Fade stop is cancelled (the loops ramp back up); otherwise the restart waits
    // for the park.
    void restart() {
        if (tp_ == Transport::Stopping) {
            if (stopMode_ != StopMode::Instant && !lg_.ended()) { tp_ = Transport::Running; lg_.start(true, 0, ramp_); }
            else restartQueued_ = true;
            return;
        }
        if (tp_ != Transport::Stopped) return;
        if (pend_.kind == Pending::Restart) { pend_ = {}; return; }
        double ph;
        const uint32_t since = gridPhase(ph) ? uint32_t(ph) : 0;
        if (since > msToSamples(kSyncGraceMs, sr_)) { pend_ = {Pending::Restart, -1}; return; }
        start(since);
    }

    // Sync = Bars: MOD's transport started (the plugin calls it on the edge, before process()). atLine: on a bar line,
    // at this block's first sample; else on the next line. Running: every track retriggers (see retrigger()); a later
    // line makes it the waiting action, or joins the one waiting (it goes first on the line), and a press or C keeps
    // it: only a stop or Sync off cancels it. Stopped: it starts from the top, or the restart waits for the line.
    // Stopping: a stop still sounding (End of loop, Fade) is cancelled as B cancels it, then as Running; else it
    // restarts at the park, on MOD's grid (see restart()). A first recording ignores it, and so do empty tracks.
    void modStarted(bool atLine) {
        if (!enabled_ || recTrack_ >= 0 || allEmpty()) return;
        if (tp_ == Transport::Stopping) { restart(); if (tp_ != Transport::Running) return; }
        if (tp_ == Transport::Stopped) {                 // only a restart can wait: kept, never toggled
            if (atLine) { pend_ = {}; start(0); } else pend_ = {Pending::Restart, -1};
            return;
        }
        if (!atLine) { if (!pend_.kind) pend_.kind = Pending::Retrigger; pend_.retrig = true; return; }
        pend_.retrig = false;
        if (pend_.kind == Pending::Retrigger) pend_ = {};
        retrigger();
    }
    // MOD's transport stopped: a stop with the Stop Mode. A first recording closes on the last line passed, or under a
    // bar is dropped (see closeFirst()).
    void modStopped(StopMode mode, float fadeMs) { stop(mode, fadeMs); }

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
    // At most kMinLoopMs (kHistoryMs before it grew for Sync): startDub's write position needs a back-date within 2 L.
    void setTriggerLatencyMs(float ms) { trigMs_ = std::max(0.0f, std::min(ms, kMinLoopMs)); }
    void setRecToDub(bool on) { recToDub_ = on; }
    void setFixedBars(uint32_t n) { fixedBars_ = n; }   // Length: 0 = free
    void setAdvanceMs(float ms) { advMs_ = std::max(0.0f, std::min(ms, 30.0f)); }   // earlier only (R7)
    void setAutoRec(bool on) {                           // off: nothing starts by itself
        autoRec_ = on;
        if (!on) { armed_ = -1; if (pend_.kind == Pending::Arm) cancelWaiting(); }
    }
    void setThresholdDb(float db) {                      // NaN -> -60
        thr_ = std::pow(10.0f, std::max(-60.0f, std::min(db, -10.0f)) / 20.0f);
    }
    // Sync = Bars (off: no grid, everything as before). setGrid comes once per block, before process(): MOD's bar
    // grid at the block start. toNextBar: samples to the next bar line, in [0, samplesPerBar). A bar outside
    // kMinLoopMs..kMaxLoopSeconds is no grid. Off: the grid is ignored and nothing kept up; MOD's waiting retrigger
    // goes (off: don't follow MOD), a waiting press acts at once.
    void setSync(bool on) {
        sync_ = on;
        if (on) return;
        gValid_ = false;
        pend_.retrig = false;
        if (pend_.kind == Pending::Retrigger) pend_ = {};
    }
    void setGrid(bool valid, bool playing, double samplesPerBar, double toNextBar) {
        gValid_ = sync_ && valid && samplesPerBar >= minLoop_ && samplesPerBar <= maxRec_;   // NaN fails
        gPlaying_ = playing;
        spb_ = samplesPerBar;
        modPh_ = toNextBar > 0.0 && toNextBar <= spb_ ? spb_ - toNextBar : 0.0;
    }

    // Not clamped here (the plugin's parameter range does that); the pool clamps to what it reserved.
    // Lowering it below the current use only takes effect at the next allocation.
    void setBudgetMb(float mb) { pool_.setBudgetBytes(mb > 0.0f ? size_t(double(mb) * 1048576.0) : 0); }   // NaN -> 0
    size_t undoBytes() const { return size_t(pool_.undoUsed()) * BlockPool::kBlock * sizeof(float); }
    // A write of track audio found the pool full with no undo layer left to give up; until a block comes back.
    bool memoryFull() const { return pool_.full(); }
    uint32_t blocksUsed() const { return pool_.used(); }

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
    // The layers heard now: 0 empty, else the recording plus its dubs still applied (one in progress, and those the
    // budget merged into the base, count). It follows the presses: an undo counts at once, not as its pass applies it.
    uint32_t layerCount(int t) const {
        if (state(t) == TrackState::Empty) return 0;
        const Track& tr = tracks_[t];
        return 1 + tr.baked + tr.undoTop + (tr.state == TrackState::Overdubbing ? 1 : 0);
    }
    uint32_t pendingOps(int t) const { return valid(t) ? tracks_[t].nOps : 0; }
    uint32_t loopLength() const { return L_; }
    uint32_t position() const { return pos_; }
    // A whole number of loops; 0 while it records, and once it is empty with no base kept for redo.
    uint32_t trackLength(int t) const {
        if (!valid(t) || tracks_[t].state == TrackState::Recording) return 0;
        return tracks_[t].state != TrackState::Empty || tracks_[t].baseUndone ? tracks_[t].len : 0;
    }
    // Samples since the track's beginning, the pass start where it started (so it is position() mod L). Recording onto
    // the loop it still grows: the base phase at the start plus the samples since.
    uint32_t trackPosition(int t) const {
        if (state(t) == TrackState::Recording) return tracks_[t].len ? tracks_[t].phase + tracks_[t].pos : 0;
        return trackLength(t) ? (tracks_[t].pos + tracks_[t].phase) % tracks_[t].len : 0;
    }
    Transport transport() const { return tp_; }
    int armedTrack() const { return armed_; }            // -1: none
    bool waiting() const { return pend_.kind; }          // an action waits for the next bar line
    // The wait is the user's own: a press or arm on t, or a restart (not MOD's lone retrigger). No t: on any track.
    bool userWaiting() const { return pend_.kind && pend_.kind != Pending::Retrigger; }
    bool userWaiting(int t) const { return pend_.kind == Pending::Restart || ((pend_.kind == Pending::Press || pend_.kind == Pending::Arm) && pend_.t == t); }
    int waitingTrack() const { return pend_.t; }         // its track; -1: none, or a restart
    uint32_t bars() const { return L_ ? bars_ : 0; }     // the first loop's bars (1 if recorded without a grid)
    // What L would be if closed now (with a grid: before the rounding to bars). An arm's start can be back-dated less
    // than the close: 0 until rec_ passes recD_.
    uint32_t recordedSamples() const { return recTrack_ >= 0 && rec_ > recD_ ? rec_ - recD_ : 0; }
    double sampleRate() const { return sr_; }
    bool enabled() const { return enabled_; }

private:
    static constexpr uint32_t kOpSlots = 2 * (kMaxPendingOps + 1);
    struct Layer { Delta d; uint32_t seq = 0; uint8_t refs = 0; };   // refs: pending ops reading d
    struct Op {                          // pending undo/redo: adds sign*d at the playhead over the len positions it owes
        Delta d;                         // a copy of the handle: the blocks are shared with the layer
        DeltaCursor c;
        float sign = 0.0f;
        uint32_t at = 0;                 // the next position it owes: it applies only when the playhead is there
        uint32_t done = 0, len = 0;      // positions applied; ramps for ramp_, ends at len (at most one pass)
        int32_t layer = -1;              // its layer while d is in a stack, else -1
        bool owns = false, ramp = false; // owns: releases d when it ends (see endOp)
        bool zero = false;               // a zero op: sets buf = 0 instead (the unrecorded rest of a recording onto L)
        bool fold = false;               // a fold op: adds buf[at + len] instead (a rounded-down close's overshoot)
    };
    struct Writer {                      // writes the overdub (or a recording onto the loop), or its punch-out fade
        int32_t layer = -1;              // index in layers; -1 = no delta (a recording onto the loop: the base)
        uint32_t w = 0, n = 0;           // write position; samples written (delta offset = n mod L)
        uint32_t left = 0;               // punch-out fade samples to go (0 = idle)
        float env = 0.0f, peak = 0.0f;   // last envelope; peak |v| written
        DeltaCursor c;
        bool cont = false, lost = false; // cont: envelope = 1 - what others write (Rec->Dub, handover); lost: pool full
        bool offset = true;              // written at the Offset behind the playhead (see startDub)
        bool full = false;               // a write was refused: memory full (see write())
    };
    struct Track {
        TrackBuf buf;                    // the base, plus every layer still in the undo stack
        Layer* layers = nullptr;         // [0, undoTop) undo stack, [undoTop, redoTop) redo stack, top at undoTop
        uint32_t undoTop = 0, redoTop = 0;
        uint32_t baked = 0;              // dubs left in buf without a layer (evicted, or not stored): layerCount() keeps them
        // In start order, a jump's split parts first. The spare slot lets a cancel past the cap go through. x2: one
        // is added only at nOps <= cap, and a jump (advance(): a park, a late restart, a retrigger) can split every op
        // once. Parts that split again at a later jump take the slots left; past them advance() keeps ops whole.
        Op ops[kOpSlots];
        uint32_t nOps = 0;
        Writer dub, fade;
        TrackState state = TrackState::Empty;
        // The track's cycle (a whole number of loops, 0 = none) and playhead in buf. It outlives a clear or an abort
        // until the next recording or reset(): the ramp-out and the pending ops still read it. Recording onto the loop
        // (and after its abort) len is cap_: buf is linear from the start press, index 0, and started empty.
        uint32_t len = 0, pos = 0;
        uint32_t xpos = 0;               // a retrigger's crossfade: the old playhead
        uint32_t phase = 0;              // the base phase at index 0: the track's beginning is phase samples before it
        uint32_t leadIn = 0;             // recording onto the loop: back-dated samples before index 0, at cap_ - leadIn
        uint32_t tailEnd = 0;            // > 0: the input blends into [tailAt, tailEnd) on the pass after the close
        uint32_t tailAt = 0;             // where: the playhead, on the timeline a retrigger leaves (see retrigger())
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
    // Cancels the waiting press, arm or restart. MOD's retrigger stays: only a stop or Sync off cancels it.
    void cancelWaiting() { pend_ = {pend_.retrig ? Pending::Retrigger : Pending::None, -1, pend_.retrig}; }
    bool idle() const {                                  // every track empty and ramped out
        for (const Track& tr : tracks_) if (tr.state != TrackState::Empty || tr.gain.v != 0.0f) return false;
        return true;
    }
    bool allEmpty() const {
        for (const Track& tr : tracks_) if (tr.state != TrackState::Empty) return false;
        return true;
    }
    // The longest non-empty track's position; its length goes to len. Lowest index on ties; none longer than L: pos_, L.
    uint32_t longest(uint32_t& len) const {
        uint32_t p = pos_;
        len = L_;
        for (int t = 0; t < kTracks; ++t)
            if (state(t) != TrackState::Empty && trackLength(t) > len) { len = trackLength(t); p = trackPosition(t); }
        return p;
    }
    bool overlap() const {                               // some track still takes a share of the input
        for (const Track& tr : tracks_) if (tr.tailEnd || tr.fade.left) return true;
        return false;
    }

    // Stopped: plays from the top, every playhead since samples on (see restart()); the loops fade in.
    void start(uint32_t since) {
        if (since) { advance(since); pos_ = (pos_ + since) % L_; }   // stopped: L_ is set
        tp_ = Transport::Running;
        for (Track& tr : tracks_) tr.gain.snap(target(tr));
        lg_.v = 0.0f;
        lg_.start(true, 0, fadeIn_);
    }

    // MOD started on a line, the looper running: every writer closes as a stop closes it, and every playhead moves on
    // as a park moves it (the longest track on its beginning). For fadeIn_ the old positions fade out under the new
    // ones (see crossfade()), read from the buffer only: so their pending ops are applied ahead now, as their playhead
    // would, over the punch-out fade too (a zero op applied after a closed writer's fade would wipe it). What still
    // writes on the old timeline, the punch-out fades and the first loop's tail (see writeOut()), goes on in step.
    // ponytail: the old read lacks an undo or redo's hold-back while it ramps in (pressed within kRampMs before): it
    // steps by the part held back; carry each track's adj into the crossfade if that is heard
    // ponytail: two retriggers can overlap: a waiting one fires on a block's last sample, then a stop edge and a start
    // edge on a line come in the next two blocks (the start cancels a sounding stop, see modStarted(), or B does). The
    // second drops the rest of the first's old read: a step of the share left, xf_ / fadeIn_ of the first's old - new
    // (about 0.1 at 128-frame blocks and 48 kHz, larger with smaller blocks); keep a second old read if that is heard
    void retrigger() {
        if (allEmpty()) return;                          // every track cleared while it waited
        for (int u = 0; u < kTracks; ++u) if (writing(tracks_[u])) close(u, false);
        for (Track& tr : tracks_) {
            tr.xpos = tr.pos;
            for (uint32_t k = 0; tr.len && k < fadeOut_; ++k) { commit(tr); if (++tr.pos == tr.len) tr.pos = 0; }
            tr.pos = tr.xpos;                            // the ops keep their place: a later playhead skips what's done
        }
        toTop();
        xf_ = fadeIn_;
    }
    // Per sample while a retrigger's crossfade runs: linear, the new positions in as a fade-in, the old ones out.
    float crossfade(float wet) {
        const float g = float(xf_--) / float(fadeIn_);
        float old = 0.0f;
        for (Track& tr : tracks_) {
            old += tr.gain.v * tr.buf.get(pool_, tr.xpos);
            if (++tr.xpos == tr.len) tr.xpos = 0;
        }
        return (1.0f - g) * wet + g * old;
    }

    // The bar grid at the next sample; false if there is none (Sync off, or no valid grid). ph: samples since the last
    // line, fractional; the sample is on a line when ph < 1. The grid is MOD's bars while it plays, else the loop's
    // bars, else (nothing recorded) the press is the line. The first recording counts lines from its start (a line)
    // unless it started on MOD's grid and MOD still plays: a recording started with MOD stopped keeps that grid, and
    // one started with no grid has none (it closes free, as with Sync off).
    bool gridPhase(double& ph) const {
        if (!gValid_ || (recTrack_ >= 0 && recNoGrid_)) return false;   // also false with Sync off (see setSync)
        if (recTrack_ >= 0 && (recFree_ || !gPlaying_)) ph = phaseIn(rec_, spb_);
        else if (gPlaying_) ph = phaseIn(modPh_ + at_, spb_);
        else if (L_) ph = phaseIn(pos_, double(L_) / bars_);
        else ph = 0.0;
        return true;
    }
    // p mod bar in [0, bar): a few scalar ops (no fmod), as gridPhase runs per sample while a press waits.
    static double phaseIn(double p, double bar) {
        const double r = p - std::floor(p / bar) * bar;
        return r > 0.0 && r < bar ? r : 0.0;             // rounding past either end: on the line
    }

    // An arm fires (its threshold crossing, or the line it waited for), back-dated by the pre-roll. With a grid it goes
    // on a line as a press does: up to kSyncGraceMs after one it starts there, the pre-roll a lead-in just before it;
    // earlier, it waits for the next line (it is then the waiting action).
    void fireArm(int t) {
        const uint32_t P = msToSamples(kPreRollMs, sr_);
        double ph;
        const bool grid = gridPhase(ph);
        const uint32_t since = grid ? uint32_t(ph) : 0;
        if (since > msToSamples(kSyncGraceMs, sr_)) { pend_ = {Pending::Arm, t, pend_.retrig}; return; }
        if (L_) startDub(tracks_[t], since + P, false, true, true, true); else startFirst(t, since + P, grid ? P : 0);
    }

    // Back-date by back (<= histLen_): the last back inputs from the history ring become buf[0..back). Without a grid
    // the close is back-dated by the trigger latency recD_ whatever back was (a press: the same; an arm: the pre-roll).
    // lead: the first lead of them are a lead-in before the start line (an arm's pre-roll on a grid). They go aside to
    // the buffer's end, so buf[0] is the line, and closeFirst() adds them just before the loop's end. The fade-in is
    // theirs. The buffer is empty: L unset, every track was reset or threw its take away (see undo()).
    void startFirst(int t, uint32_t back, uint32_t lead = 0) {
        for (uint32_t i = 0; i < back; ++i) {
            const float v = hist_[(histW_ + histLen_ - back + i) % histLen_] * fadeInGain(i);
            if (float* s = sample(tracks_[t], i < lead ? cap_ - lead + i : i - lead)) *s = v;
        }
        rec_ = back - lead;
        recLead_ = lead;
        recFadeIn_ = lead ? 0 : fadeIn_;                 // the lead-in had the fade-in (lead = the pre-roll > fadeIn_)
        recD_ = msToSamples(trigMs_, sr_);               // <= minLoop_ <= histLen_ (trigMs_ <= kMinLoopMs)
        recTrack_ = t;
        recFree_ = gValid_ && !gPlaying_;
        recNoGrid_ = !gValid_;
        tracks_[t].state = TrackState::Recording;
    }

    // Ends u's recording or overdub as its B press would. True if u still takes a share of the input after it
    // (a tail or punch-out fade): a track starting now takes the complement.
    bool close(int u, bool toDub) {
        Track& tr = tracks_[u];
        if (recTrack_ == u) return closeFirst(toDub);
        const bool base = tr.state == TrackState::Recording;   // a recording onto the loop
        if (base) closeOntoLoop(tr);
        endDub(tr, false);
        if (base && toDub) startDub(tr, 0, true, false, tr.fade.offset);   // Rec->Dub: the fade input splits g / 1-g
        return tr.fade.left > 0;
    }

    // A recording onto the loop stops at the press, n samples after its start (the playhead): it is k whole loops long
    // counted from the start, rounded down within kRoundDownFraction of a loop past them, never below one. From now on
    // its writer (the punch-out fade) and playhead wrap at len. Nothing long happens here: rounded up, a zero op clears
    // the unrecorded rest as the playhead reaches it; rounded down, a fold op adds what the writer put past len onto
    // the beginning on the next pass. Only the lead-in kept aside is placed now, at the end, just before index 0.
    void closeOntoLoop(Track& tr) {
        const uint32_t n = tr.pos, L = L_, b = tr.leadIn;
        const uint32_t k = n < L ? 1 : float(n % L) <= kRoundDownFraction * float(L) ? n / L : n / L + 1;
        const uint32_t len = k * L, a = (n + cap_ - tr.dub.w) % cap_;   // a: the writer's lag (the offset)
        tr.len = len;
        tr.pos = n % len;
        tr.dub.w = (n + len - a) % len;                 // index n - a, wrapped
        const uint32_t from = len - std::min(b, len);   // the lead-in covers [from, len): all of it if b > len
        if (n >= from) tr.gain.snap(0.0f);              // the playhead lands on recorded audio, mid-wave: ramp in (R7)
        if (n < len) {                                  // rounded up: [n, len) was never recorded
            const uint32_t z = std::max(n, from);
            tr.buf.zero(pool_, z, len);                 // [z, len) now, under the lead-in; [n, z) on the way
            addRaw(tr, n, z - n, false);
        } else if (n > len + a) {
            addRaw(tr, 0, n - a - len, true);           // [len, n - a) onto [0, n - a - len)
        }
        for (uint32_t i = 0; i < b; ++i)                // index i - b
            if (float* s = sample(tr, (i + len - b % len) % len)) *s += tr.buf.get(pool_, cap_ - b + i);
    }

    // Without a grid the close is back-dated by the trigger latency: L = r - recD_. With one it lands on the last line
    // passed (a press past the grace window waited for the next): L is whole bars of the bar length now, counted from
    // the start line, and under one bar (a double press, a stop before the first line) drops the recording. Either way
    // the D samples recorded past L are its tail and playback goes on from r: D is the back-date unless the tempo
    // changed during the recording. Rounded up past r, there is no tail and the rest plays silence. A lead-in (see
    // startFirst) goes just before the loop's end. True if a writer back-dated as the press was takes exactly the rest
    // (cont); false if dropped, or if the tempo changed: D is then not that back-date, and the writer has to fade in
    // under the rest instead (trim).
    bool closeFirst(bool toDub) {
        Track& tr = tracks_[recTrack_];
        uint32_t L = recordedSamples(), bars = 1;        // rec_ - recD_, never below 0
        bool exact = true;                               // D is the press's back-date
        double ph;
        if (gridPhase(ph)) {
            const uint32_t span = rec_ - std::min(rec_, uint32_t(ph));   // up to the last line
            const double most = std::floor(maxRec_ / spb_);           // L <= maxRec_
            bars = uint32_t(std::max(1.0, std::min(std::round(span / spb_), most)));
            // The span itself when it is off bars x spb only by the lines' own rounding (a fractional bar; the tempo
            // held): round(bars x spb) can be a sample past a waited close, which would leave no tail, only a dip
            const double b = bars * spb_;
            L = span < minLoop_ ? 0 : std::fabs(span - b) < 2.0 ? span : uint32_t(std::lround(b));
            exact = L == span;
        }
        recTrack_ = -1;
        if (L < minLoop_) { tr.state = TrackState::Empty; tr.buf.clear(pool_); return false; }   // an accidental double press
        const uint32_t D = rec_ > L ? rec_ - L : 0, T = std::min(std::max(msToSamples(tailMs_, sr_), fadeIn_), L / 2);
        for (uint32_t i = 0; i < std::min(D, T); ++i)                 // fold the tail recorded so far (tail 0: 3 ms)
            if (float* s = sample(tr, i)) *s += tr.buf.get(pool_, L + i) * float(T - i) / float(T);
        const uint32_t n = std::min(D, histLen_);                     // the share the last n inputs went to the base
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t k = D - n + i;
            histUsed_[(histW_ + histLen_ - n + i) % histLen_] = k < T ? float(T - k) / float(T) : 0.0f;
        }
        tr.tailEnd = rec_ >= L && D < T ? T : 0;                      // the rest goes straight in while playing
        const uint32_t lead = recLead_, z = std::max(rec_, L - lead); // a lead-in covers [L - lead, L)
        if (rec_ < L) {                                 // rounded up: fade out, shares too; a zero op clears the rest
            for (uint32_t i = 0; i < std::min(fadeIn_, rec_); ++i) {
                const float g = float(i) / float(fadeIn_);
                if (float* s = sample(tr, rec_ - 1 - i)) *s *= g;
                histUsed_[(histW_ + histLen_ - 1 - i) % histLen_] *= g;   // fadeIn_ <= histLen_
            }
            tr.buf.zero(pool_, z, L);                                 // [z, L) now, under the lead-in; [r, z) later
            addRaw(tr, rec_, z - rec_, false);                        // (no ops are left on a track while L is unset)
        }
        for (uint32_t i = 0; i < lead; ++i)                           // just before the top
            if (float* s = sample(tr, L - lead + i)) *s += tr.buf.get(pool_, cap_ - lead + i);
        L_ = L;
        bars_ = bars;
        // D mod L (D can exceed L), or r if rounded up. After a tempo change during the recording D is not the
        // back-date, so the loop's top lands D - back off the close line. Accepted: keeping the recording's timeline
        // keeps the tail and the seam click-free, and a MOD start re-aligns it (aligning here would click every pass).
        pos_ = rec_ % L;
        tr.len = L;
        tr.pos = tr.tailAt = pos_;                                    // the tail, if any, starts here (pos_ = D)
        tr.phase = 0;                                                 // laid out by base phase
        tr.state = TrackState::Playing;
        // No track ramp: the loops fade in instead. Rounded up onto the lead-in's middle, though, the track ramps in.
        tr.gain.snap(rec_ < L && rec_ + lead > L ? 0.0f : target(tr));
        // Playback starts D past the recording's own fade-in: the loops fade in over the part it skips, the restart's
        // kFadeInMs once D reaches it (R10). D = 0 skips nothing.
        if (D) { lg_.v = 0.0f; lg_.start(true, 0, std::min(D, fadeIn_)); }
        // Rec->Dub: back-dated over the inputs whose share histUsed_ now holds, D (rounded up: the fade-out's length).
        // Within the history it takes exactly the rest (the tail input splits g / 1-g); past it, it fades in (trim).
        const uint32_t b = rec_ < L ? std::min(fadeIn_, rec_) : D;
        if (toDub) startDub(tr, std::min(b, histLen_), b <= histLen_, false, false, b > histLen_);
        return T > 0 && exact;
    }

    // Punch-in, back-dated by D: the last D inputs go to the D positions before the write position.
    // cont (Rec->Dub, handover): the layer takes exactly the complement of what the others write. Otherwise it
    // fades in, capped by the complement of a tail or punch-out fade still writing, so it never steps where it starts.
    // base: a recording onto the loop (L set, track Empty). It becomes the track's base, with no delta, written
    // linearly from index 0 at the press (len = cap_) until close() picks its length. The part back-dated before index
    // 0 goes aside to the buffer's end. The buffer starts empty (see wipe()): the track plays silence.
    // offset: written at the Offset. A cont writer takes the offset of the one it complements, so a one-press chain
    // stays aligned: none after the first recording (Rec->Dub, handover before L), which has no offset.
    // trim: an arm's pre-roll. Its start is apart from the press that closed the last writer, so it always takes only
    // the history's remaining share: it never re-records input that writer took, whether or not its tail still runs.
    // Likewise a writer after a first loop whose close did not match its back-date (closeFirst): it fades in under it.
    // ponytail: the emptying does not ramp; a re-record within kRampMs of a clear or base undo cuts that ramp-out
    // ponytail: a cancel or abort leaves its shares in the history; an arm firing within the pre-roll after one trims
    // that part of its pre-roll (not reachable by foot); take the shares back out on a cancel if that is hit
    void startDub(Track& tr, uint32_t D, bool cont, bool base = false, bool offset = true, bool trim = false) {
        const uint32_t back = D + (offset ? msToSamples(advMs_, sr_) : 0);   // the offset writes behind the playhead
        if (base) {                                      // a stale base or its pending ops never come back
            wipe(tr);
            tr.len = cap_; tr.pos = 0; tr.phase = pos_; tr.leadIn = back;
        } else {
            clearRedo(tr);
        }
        const uint32_t len = tr.len, p = tr.pos;
        Writer& dub = tr.dub;
        dub = Writer{};
        dub.w = (p + 2 * len - back) % len;
        dub.cont = cont;
        dub.offset = offset;
        if (!base) {                                     // base: the writer starts in the aside and wraps to 0
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
                ++tr.baked;
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

    // Commits the pending ops that owe the playhead (see commit()), then reads it through the track's gain.
    float play(Track& tr) {
        const float adj = commit(tr);
        const float g = tr.gain.next(target(tr), ramp_);
        return g != 0.0f ? g * (tr.buf.get(pool_, tr.pos) + adj) : 0.0f;
    }
    // Applies the pending ops that owe the playhead. Returns what holds back their change during their ramp.
    // A recording onto the loop reads ahead of its writer: silence, as its buffer started empty (startDub()).
    float commit(Track& tr) {
        float adj = 0.0f;                                // holds back the ops' change during their ramp
        for (uint32_t k = 0; k < tr.nOps;) {
            Op& op = tr.ops[k];
            if (op.at != tr.pos) { ++k; continue; }      // a split part waiting for its stretch
            if (op.zero) {
                tr.buf.zero(pool_, tr.pos, tr.pos + 1);
            } else if (op.fold) {
                if (float* s = sample(tr, tr.pos)) *s += tr.buf.get(pool_, tr.pos + tr.len);
            } else {
                const uint32_t off = (tr.pos + tr.len - op.d.start()) % tr.len;
                if (off < op.d.length()) {
                    const float v = op.sign * op.d.read(pool_, op.c, off);
                    if (float* s = sample(tr, tr.pos)) *s += v;
                    if (op.ramp && op.done < ramp_) adj -= v * (1.0f - float(op.done) / float(ramp_));
                }
            }
            if (++op.at == tr.len) op.at = 0;
            if (++op.done == op.len) endOp(tr, k); else ++k;
        }
        return adj;
    }

    // The outgoing shares of the input: the first recording's tail and the punch-out fade; returns their sum.
    float writeOut(Track& tr, float x) {
        float used = 0.0f;
        if (tr.tailEnd) {                                // tail of the first recording, g: 1 -> 0 over T = tailEnd
            if (float* s = sample(tr, tr.tailAt)) *s += x * float(tr.tailEnd - tr.tailAt) / float(tr.tailEnd);
            used += float(tr.tailEnd - tr.tailAt) / float(tr.tailEnd);
            if (++tr.tailAt == tr.tailEnd) tr.tailEnd = 0;
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

    // Refused (memory full), v is not written (nor stored: the delta stays in step) and the take ends at the next
    // sample (see process()); a punch-out fade just goes on.
    void write(Track& tr, Writer& wr, float v) {
        if (float* s = sample(tr, wr.w)) *s += v;
        else { v = 0.0f; wr.full = true; }
        if (wr.layer >= 0 && !wr.lost) store(tr, wr, v);
        wr.peak = std::max(wr.peak, std::fabs(v));
        if (++wr.w == tr.len) wr.w = 0;
        ++wr.n;
    }

    // Sample i of tr's buffer, writable (see TrackBuf::at). Loops win over undo: a full pool gives up undo layers for
    // it (evict()). nullptr when none is left to give up: memory full (memoryFull()).
    float* sample(Track& tr, uint32_t i) {
        float* s;
        while (!(s = tr.buf.at(pool_, i)) && evict()) {}
        return s;
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
    // the lowest seq goes. Layers read by a pending op (refs) or still written by a punch-out fade are skipped. A
    // track whose far end is still read offers no redo entry, though one nearer the top may be free (parks that move
    // the playhead can end ops out of order): it may then drop an undo layer instead. Safe, not optimal. False if
    // nothing could be freed.
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
        if (bi < tr.undoTop) { --tr.undoTop; ++tr.baked; }   // an undo layer: its sound stays in buf
        --tr.redoTop;
        return true;
    }

    void clearRedo(Track& tr) {
        for (uint32_t i = tr.undoTop; i < tr.redoTop; ++i) dropLayer(tr, i);
        tr.redoTop = tr.undoTop;
    }

    // Layer i left both stacks: release its blocks, or hand them to the newest op still reading them (endOp passes
    // them on if another reader ends later).
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
        tr.undoTop = tr.redoTop = tr.baked = 0;
        tr.dub = tr.fade = Writer{};
        tr.tailEnd = 0;
        tr.baseUndone = false;
    }

    // dropLayers, the pending ops too, and the buffer: the track is empty and holds no blocks.
    void wipe(Track& tr) {
        dropLayers(tr);                                  // first: it hands layers still read to their ops
        for (uint32_t k = 0; k < tr.nOps; ++k) if (tr.ops[k].owns) tr.ops[k].d.release(pool_);
        tr.nOps = 0;
        tr.buf.clear(pool_);
    }

    // layer >= 0: d is that layer's delta; -1: the op owns d.
    // ponytail: an op lasts one pass of its track, up to 5 min on a long one, and holds its slot that long: presses past
    // kMaxPendingOps within one long pass are ignored; more slots if a foot ever gets there
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

    // An op without a delta: a zero op, or a fold op, owing n positions from at.
    void addRaw(Track& tr, uint32_t at, uint32_t n, bool fold) {
        if (!n) return;
        Op& op = tr.ops[tr.nOps++];
        op = Op{};
        op.zero = !fold;
        op.fold = fold;
        op.at = at;
        op.len = n;
    }

    // An owner hands its delta to another op still reading it, else releases it: parks that land a track on different
    // indices can change which reader ends last.
    void endOp(Track& tr, uint32_t k) {
        Op& op = tr.ops[k];
        if (op.owns) {
            uint32_t j = 0;
            while (j < tr.nOps && (j == k || !tr.ops[j].d.shares(op.d))) ++j;
            if (j < tr.nOps) tr.ops[j].owns = true; else op.d.release(pool_);
        } else if (op.layer >= 0) {
            --tr.layers[op.layer].refs;
        }
        std::copy(tr.ops + k + 1, tr.ops + tr.nOps, tr.ops + k);   // keep start order
        --tr.nOps;
    }

    // The stop's end: every playhead goes to the top (see toTop()). A queued restart then starts.
    void park() {
        toTop();
        tp_ = Transport::Stopped;
        if (restartQueued_) { restartQueued_ = false; restart(); }
    }

    // Every playhead moves on by the same shift, which puts the longest track on its beginning (so one-loop tracks land
    // at the top, base phase 0): a park, or a retrigger.
    void toTop() {
        uint32_t ml;
        const uint32_t mp = longest(ml);
        advance((ml - mp) % ml);
        pos_ = 0;
    }

    // Every track's playhead moves on by shift: a park's or a late restart's (stopped), or a retrigger's (its crossfade
    // covers the jump). Each track's new playhead is its P. An op whose owed stretch wraps past P splits there: a new
    // part A owes [P, wrap end) and the op keeps [at, P), with the delta ownership. P is the same index at every park
    // for one-loop tracks and those whose length divides the longest's; otherwise (or when the longest changes, after a
    // late restart or a retrigger) parts of an earlier split can split again.
    // ponytail: past the op slots (many pending ops and repeated jumps), the rest stay whole: each plays its unapplied
    // part once (a step where it ends) and finishes within two passes; more slots if that is ever heard
    void advance(uint32_t shift) {
        for (Track& tr : tracks_) {
            if (!tr.len) continue;
            const uint32_t P = (tr.pos + shift % tr.len) % tr.len;
            auto rel = [&](const Op& o) { return (o.at + tr.len - P) % tr.len; };   // owed start, counted from P
            auto wraps = [&](const Op& o) { return rel(o) != 0 && rel(o) + (o.len - o.done) > tr.len; };
            uint32_t w = 0;
            for (uint32_t k = 0; k < tr.nOps; ++k) {
                tr.ops[k].ramp = false;                  // after a jump the playhead only reads applied audio
                w += wraps(tr.ops[k]);
            }
            w = std::min(w, kOpSlots - tr.nOps);         // the first w that wrap split
            std::copy_backward(tr.ops, tr.ops + tr.nOps, tr.ops + tr.nOps + w);
            for (uint32_t k = w, a = 0; a < w; ++k) {
                Op& o = tr.ops[k];
                if (!wraps(o)) continue;
                Op& A = tr.ops[a++];
                A = o;
                A.at = P;
                A.len = rel(o) + (o.len - o.done) - tr.len;
                A.done = 0;
                A.owns = false;
                A.c = DeltaCursor{};
                if (A.layer >= 0) ++tr.layers[A.layer].refs;
                o.len = tr.len - rel(o);
                o.done = 0;
            }
            tr.nOps += w;
            tr.pos = P;
        }
    }

    bool fail() { release(); return false; }
    void release() {
        reset();                                         // returns the blocks while the pool still exists
        for (Track& tr : tracks_) { delete[] tr.layers; tr.layers = nullptr; }   // a buffer's table stays to its next init
        std::free(hist_); hist_ = nullptr;
        std::free(histUsed_); histUsed_ = nullptr;
        pool_.reserve(0);                                // frees the pool, reserves nothing
        enabled_ = false;
    }

    Track tracks_[kTracks];
    BlockPool pool_;                 // the tracks' audio (Audio) and the overdub layer deltas for undo/redo (Undo)
    float* hist_ = nullptr;          // input history ring, for back-dating presses
    float* histUsed_ = nullptr;      // per history input: the envelope share already written somewhere (splits)
    uint32_t histLen_ = 0, histW_ = 0;
    double sr_ = 48000.0;
    uint32_t cap_ = 0;               // samples per track buffer: maxRec_ plus 1 s of slack (the lead-in aside)
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
    struct Pending {                 // Sync: the action waiting for the next bar line: a press or an arm firing on
        enum Kind : uint8_t { None, Press, Arm, Restart, Retrigger } kind = None;   // track t, a restart (t -1), or
        int t = -1;                  // only MOD's retrigger (t -1)
        bool retrig = false;         // MOD's retrigger goes first on that line (see modStarted())
    };
    Pending pend_;
    uint32_t recLead_ = 0;           // first recording: its lead-in before buf[0], kept aside at the buffer's end
    uint32_t recFadeIn_ = 0;         // first recording: its fade-in length from buf[0] (fadeIn_, or 0 after a lead-in)
    uint32_t bars_ = 1;              // the first loop's bars
    uint32_t fixedBars_ = 0;         // Length: the first loop closes itself after this many bars (0: free)
    bool sync_ = false, gValid_ = false, gPlaying_ = false;   // Sync = Bars; MOD's grid: usable, transport playing
    bool recFree_ = false;           // the first recording started on the free grid (MOD stopped): it keeps it
    bool recNoGrid_ = false;         // the first recording started with no grid: it keeps none
    double spb_ = 0.0, modPh_ = 0.0; // MOD's samples per bar, and samples since its last line at the block start
    uint32_t xf_ = 0;                // a retrigger's crossfade: samples to go
    uint32_t at_ = 0;                // the sample process() is at in its block, while it asks gridPhase (else 0)
    float thr_ = 0.01f;              // auto-record threshold, linear (-40 dB)
    bool autoRec_ = false;
    bool recToDub_ = false, enabled_ = false, dryThru_ = false, bypass_ = false;
};

} // namespace ouro
