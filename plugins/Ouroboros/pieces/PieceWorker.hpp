#pragma once
// The pieces worker thread: it writes a held snapshot to a slot folder while the audio thread plays on, and loads a
// slot into the engine once the audio thread lends it.
//
// Handoff, all through state_ (release on each store that hands something over, acquire on each load that takes it):
//   audio thread: Idle -> Save | Load (start*: the job's fields written before), WantEngine -> Lent (lendEngine: no
//                 engine call after it), Done -> Idle (poll: result_ read, then the engine is the audio thread's again)
//   worker:       Load -> WantEngine (every check passed), * -> Done (result_ and loaded_ written before)
//   halt():       Done -> Idle, with no audio thread running; the worker turns WantEngine back to Load itself.
// The audio thread's calls are wait-free: atomics only. cancel_ and halt_ are flags the worker reads between chunks.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <utility>
#include "extra/Thread.hpp"
#include "dsp/Looper.hpp"
#include "pieces/PieceFile.hpp"

namespace ouro { namespace pieces {

class PieceWorker : public DISTRHO::Thread {
public:
    enum class Result : int { None, Saved, DiskFull, SaveError, Cancelled, Loaded, Empty, TooBig, LoadError };

    // Non-RT. root: piecesDir(); "" when pieces are unavailable (a save fails, a load finds nothing, no file call).
    PieceWorker(Looper& engine, std::string root) : engine_(engine), root_(std::move(root)) { startThread(); }
    ~PieceWorker() override { halt(); stopThread(-1); }

    // Audio thread.
    bool idle() const { return state_.load(std::memory_order_acquire) == Idle; }
    // The snapshot stays held until poll() returns the result; refusals: engine.audioRefusals() at the snapshot.
    void startSave(int slot, const PieceMeta& m, uint32_t refusals) {
        if (!idle()) return;
        slot_ = slot; job_ = m; refusals_ = refusals;
        cancel_.store(false, std::memory_order_relaxed);
        state_.store(Save, std::memory_order_release);
    }
    void watch(uint32_t refusals) {   // per block: a save cancels itself once the pool refused an audio block
        if (state_.load(std::memory_order_relaxed) == Save && refusals != refusals_) cancel();
    }
    void startLoad(int slot) {        // the engine's rate, capacity and track limit read now
        if (!idle()) return;
        slot_ = slot;
        rate_ = uint32_t(std::lround(engine_.sampleRate()));
        capacity_ = engine_.capacityBlocks();
        maxLen_ = engine_.maxTrackLength();
        progress_.store(0.0f, std::memory_order_relaxed);
        state_.store(Load, std::memory_order_release);
    }
    bool wantsEngine() const { return state_.load(std::memory_order_acquire) == WantEngine; }
    void lendEngine() {               // from now until poll() returns the load's result: no engine call
        int s = WantEngine;
        state_.compare_exchange_strong(s, Lent, std::memory_order_release, std::memory_order_relaxed);
    }
    void cancel() { cancel_.store(true, std::memory_order_relaxed); }   // a save stops between chunks, drops NN.new
    Result poll() {                   // the finished job's result once, else None
        if (state_.load(std::memory_order_acquire) != Done) return Result::None;
        const Result r = result_;
        state_.store(Idle, std::memory_order_release);
        return r;
    }
    float progress() const { return progress_.load(std::memory_order_relaxed); }   // 0..1 of the load's samples
    const PieceMeta& loaded() const { return loaded_; }                            // after Loaded, until the next load
    uint32_t fullSlots() const { return full_.load(std::memory_order_relaxed); }   // as of the last job (bit s-1)

    // Non-RT, no audio thread running (activate, sampleRateChanged, destructor): the job stops (a save drops NN.new, a
    // load not lent ends untouched, a lent one empties the engine), its result is dropped; returns once idle.
    void halt() {
        halt_.store(true, std::memory_order_relaxed);
        for (int s; (s = state_.load(std::memory_order_acquire)) != Idle; d_msleep(1))
            if (s == Done || !isThreadRunning()) { state_.store(Idle, std::memory_order_relaxed); break; }
        halt_.store(false, std::memory_order_relaxed);
    }

private:
    enum : int { Idle, Save, Load, WantEngine, Lent, Done };
    static constexpr uint32_t kChunk = 4096;

    void run() override {
        if (!root_.empty()) recoverSlots(root_);
        rescan();
        while (!shouldThreadExit()) {
            const int s = state_.load(std::memory_order_acquire);
            if (s != Save && s != Load) { d_msleep(20); continue; }
            result_ = s == Save ? save() : load();
            rescan();                 // whatever the result: a commit can fail after its swap
            state_.store(Done, std::memory_order_release);
        }
    }
    void rescan() { if (!root_.empty()) full_.store(scanSlots(root_), std::memory_order_relaxed); }
    bool saveStopped() const { return cancel_.load(std::memory_order_relaxed) || halt_.load(std::memory_order_relaxed); }

#ifdef _WIN32
    Result save() { return Result::SaveError; }   // no pieces folder
    Result load() { return Result::Empty; }
#else
    static Result saveResult(Err e) { return e == Err::None ? Result::Saved : e == Err::NoSpace ? Result::DiskFull : Result::SaveError; }

    // Only readSnapshot() on the engine; the lengths are job_'s.
    Result save() {
        if (root_.empty()) return Result::SaveError;
        const std::string dir = stagingDir(root_, slot_);
        Result r = writeSlot(dir);
        if (r == Result::Saved && saveStopped()) r = Result::Cancelled;
        if (r == Result::Saved) r = saveResult(commitSlot(root_, slot_));
        if (r != Result::Saved) removeTree(dir);
        return r;
    }
    Result writeSlot(const std::string& dir) {
        for (int p = 0; p < kParts; ++p)
            for (int t = 0; t < kTracks; ++t) {
                const uint32_t len = job_.track[p][t].len;
                if (!len) continue;
                WavWriter w;
                if (!w.open(dir + "/" + trackName(p, t) + ".wav", job_.rate)) return saveResult(errFromErrno());
                for (uint32_t i = 0; i < len; i += kChunk) {
                    if (saveStopped()) return Result::Cancelled;
                    const uint32_t n = std::min(len - i, uint32_t(kChunk));
                    engine_.readSnapshot(p, t, i, buf_, n);
                    const Err e = w.write(buf_, n);
                    if (e != Err::None) return saveResult(e);
                }
                const Err e = w.close();
                if (e != Err::None) return saveResult(e);
            }
        const std::string text = formatPiece(job_);
        const int fd = ::open((dir + "/piece.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) return saveResult(errFromErrno());
        Err e = writeAll(fd, text.data(), text.size()) && fsync(fd) == 0 ? Err::None : errFromErrno();
        if (::close(fd) != 0 && e == Err::None) e = errFromErrno();
        return saveResult(e);
    }

    // No engine call until lent; then the worker's until Done. Each track read within its len from piece.txt.
    Result load() {
        if (root_.empty()) return Result::Empty;
        const std::string dir = slotDir(root_, slot_);
        if (!exists(dir + "/piece.txt")) return Result::Empty;
        char text[8192];              // piece.txt is under 1 KB; a larger file is not one
        FILE* f = fopen((dir + "/piece.txt").c_str(), "rb");
        const size_t got = f ? fread(text, 1, sizeof text, f) : 0;
        const bool ok = f && !ferror(f) && got < sizeof text;
        if (f) fclose(f);
        PieceMeta m;
        if (!ok || !parsePiece(std::string(text, got), m) || m.rate != rate_ || m.loop > maxLen_) return Result::LoadError;
        WavReader wav[kParts][kTracks];
        uint64_t total = 0;
        for (int p = 0; p < kParts; ++p)
            for (int t = 0; t < kTracks; ++t) {
                const uint32_t len = m.track[p][t].len;
                if (len > maxLen_) return Result::LoadError;   // before blocksFor(), which it could overflow
                if (len && (!wav[p][t].open(dir + "/" + trackName(p, t) + ".wav") || wav[p][t].rate() != m.rate ||
                            wav[p][t].frames() != len)) return Result::LoadError;
                total += len;
            }
        if (blocksFor(m) > capacity_) return Result::TooBig;
        if (!borrowEngine()) return Result::Cancelled;
        engine_.importBegin();
        uint64_t done = 0;
        for (int p = 0; p < kParts; ++p)
            for (int t = 0; t < kTracks; ++t)
                for (uint32_t i = 0, len = m.track[p][t].len; i < len; i += kChunk) {
                    const uint32_t n = std::min(len - i, uint32_t(kChunk));
                    const bool halted = halt_.load(std::memory_order_relaxed);
                    if (halted || !wav[p][t].read(buf_, n) || !engine_.importTrack(p, t, i, buf_, n)) {
                        engine_.importBegin();   // every part empty
                        return halted ? Result::Cancelled : Result::LoadError;
                    }
                    done += n;
                    progress_.store(float(double(done) / double(total)), std::memory_order_relaxed);
                }
        engine_.importEnd(m);
        loaded_ = m;
        progress_.store(1.0f, std::memory_order_relaxed);
        return Result::Loaded;
    }
    bool borrowEngine() {             // false: halted before the audio thread lent it
        state_.store(WantEngine, std::memory_order_release);
        for (;; d_msleep(1)) {
            if (state_.load(std::memory_order_acquire) == Lent) return true;
            int s = WantEngine;
            if (halt_.load(std::memory_order_relaxed) &&
                state_.compare_exchange_strong(s, Load, std::memory_order_relaxed)) return false;
        }
    }
#endif

    Looper& engine_;
    const std::string root_;
    std::atomic<int> state_{Idle};
    std::atomic<bool> cancel_{false}, halt_{false};
    std::atomic<float> progress_{0.0f};
    std::atomic<uint32_t> full_{0};
    // The job, written by the audio thread before it publishes Save or Load.
    int slot_ = 0;
    PieceMeta job_;
    uint32_t refusals_ = 0, rate_ = 0, capacity_ = 0, maxLen_ = 0;
    // Written by the worker before it publishes Done.
    Result result_ = Result::None;
    PieceMeta loaded_;
    float buf_[kChunk];
};

} }
