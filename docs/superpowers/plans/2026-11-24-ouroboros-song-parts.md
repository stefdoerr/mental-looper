# Song Parts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 4 parts of 4 tracks each, switched seamlessly at the part's end, plus a 16-step song that switches parts by itself, all on pooled track storage so 16 tracks fit in the Dwarf's memory.

**Architecture:** Track audio moves from one 5-minute `calloc` per track into the existing `BlockPool`, behind a small `TrackBuf` type (blocks allocated on first write, shared by reference count for copies). The engine keeps `kParts x kTracks` tracks and a current part; every existing per-track action and getter acts on the current part, so today's behaviour is part A's. The song is engine state advanced at part ends. The plugin adds the controls (Part, Song, the step proxies, 32 hidden step ports) and the widgets show parts and the song.

**Tech Stack:** C++14 header-only engine (`plugins/Ouroboros/dsp/`), DPF LV2 plugin (`OuroborosPlugin.hpp`), Starless widgets (`hmi/`), tests in `tests/` (`make test`, `make test-lv2`).

**Spec:** `docs/superpowers/specs/2026-11-24-ouroboros-song-parts-design.md` (approved 2026-11-24). Its "My call" items are binding.

## Global Constraints

- Real-time safe: no allocation, locks, syscalls or unbounded work in `run()`/`process()`. Allocation only in `init()`/constructor/`sampleRateChanged()`.
- CPU rule: playing part A only (no parts or song used) costs what `main` costs, within measurement noise, measured with the bench on x86 and on the Dwarf. With Sync Off and Song Off the per-sample path gains at most flag tests beyond the storage indirection.
- Existing parameter symbols are frozen; new controls go before Bypass. Saved pedalboards connect by symbol.
- Undo Budget keeps its symbol (`budget`), range (16-400 MB), default (200 MB) and meaning: it caps undo layers only.
- The pool is as large as everything reserved today: `kTracks x cap_ x 4 B + kMaxBudgetMb MB` (about 630 MB at 48 kHz), with a fallback to smaller sizes if the reservation fails.
- `kParts = 4`, `kSteps = 16`; repeats 1-16; step parts A-D, Loop, Stop.
- Any DPF change stays in the local patch; never proposed upstream.
- Every commit message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **A switch while a long track of the outgoing part is mid-pass:** the switch waits for the longest track's end (Task 2, `partLengthIsTheLongestTrack`).
2. **A copy on entry must never half-happen:** with shared blocks it allocates nothing (Task 2, `copyOnEntryAllocatesNothing`).
3. **MOD's start (Sync on) while a switch is queued:** the retrigger plays the current part from its top and the switch stays queued for that part's end (Task 2, `modStartKeepsAQueuedSwitch`).
4. **Undo right after a switch, on a copied track:** the one layer undoes to empty and redoes back (Task 2, `undoOnACopiedTrack`).
5. **Clear Track empties a part's last track, then the part is left and entered again:** it is filled again on entry (Task 2, `clearedPartRefillsOnEntry`).

---

### Task 1: Pooled track storage

**Files:**
- Create: `plugins/Ouroboros/dsp/TrackBuf.hpp`
- Modify: `plugins/Ouroboros/dsp/BlockPool.hpp`, `plugins/Ouroboros/dsp/Delta.hpp` (alloc kind), `plugins/Ouroboros/dsp/Looper.hpp` (every `tr.buf` use, `init`, `release`)
- Create: `tests/test_memory.cpp`, `tests/bench/bench.cpp` (from `/tmp/claude-1000/-home-sdoerr-Fun/21b65a11-f638-4649-83da-0fee50c4ce8c/scratchpad/bench/bench.cpp`)
- Modify: `tests/looper_rig.hpp` (pool size argument), `Makefile` (test list, if tests are listed)

**Interfaces:**
- Produces, `BlockPool`:
  - `enum class Kind : uint8_t { Undo, Audio };`
  - `int32_t alloc(Kind k = Kind::Undo);` Undo blocks count toward the budget; Audio blocks only toward the reserve. -1 when refused.
  - `void addRef(int32_t b);` and `bool shared(int32_t b) const;`. `free(b)` drops one reference and frees at zero.
  - `uint32_t used() const;` (all blocks), `uint32_t undoUsed() const;`
- Produces, `TrackBuf` (in `namespace ouro`):
  - `bool init(uint32_t cap);` non-RT; a block table for `cap` samples, no audio.
  - `float get(const BlockPool& p, uint32_t i) const;` 0 where nothing was written.
  - `float* at(BlockPool& p, uint32_t i);` writable sample: allocates its block (Audio) or unshares it (copies the 4096 samples) first. `nullptr` if the pool refuses.
  - `void zero(BlockPool& p, uint32_t from, uint32_t to);` zeroes `[from, to)`: whole blocks are released, partial ones written.
  - `void clear(BlockPool& p);` releases every block.
  - `void shareFrom(BlockPool& p, const TrackBuf& src);` this buffer becomes a copy of `src` by reference (no allocation).
- Produces, `Looper`:
  - `bool init(double sampleRate, uint32_t maxBlock, size_t poolBytes = 0);` 0 = the default size with its fallback (Global Constraints); a nonzero size is used as given (tests pass a small pool).
  - `bool memoryFull() const;` true from a refused audio write until a block is freed.
  - `uint32_t blocksUsed() const;` the pool's used blocks (tests).

- [ ] **Step 1: Write the failing tests** in `tests/test_memory.cpp` (add `Rig(double rate, uint32_t block, size_t poolBytes)` to `looper_rig.hpp`, calling `lp.init(rate, block, poolBytes)`):

```cpp
static void onlyRecordedAudioUsesThePool() {
    Rig r; const uint32_t before = r.lp.blocksUsed();
    recordBase(r, 48000, 0.1f);
    CHECK(r.lp.blocksUsed() - before <= 48000 / 4096 + 3);              // about one second's blocks, not 5 minutes
    r.lp.clearTrack(0); r.silence(4800);
    CHECK(r.lp.blocksUsed() == before);
}
static void aFullPoolClosesTheFirstRecording() {
    Rig r(48000.0, 128, size_t(8) << 20);                               // 8 MB: about 43 s of audio
    r.lp.pressRec(0); r.feed(sine(220, 0.1f, 50 * 48000));
    CHECK(r.lp.state(0) == TrackState::Playing && r.lp.memoryFull());
    CHECK(r.lp.loopLength() > 30 * 48000 && r.lp.loopLength() < 44 * 48000);
    r.lp.clearTrack(0); r.silence(4800);
    CHECK(!r.lp.memoryFull());
}
static void aFullPoolClosesARecordingOntoTheLoop() {
    Rig r(48000.0, 128, size_t(8) << 20);
    recordBase(r, 20 * 48000, 0.1f);                                    // about half the pool
    r.lp.pressRec(1); r.feed(sine(330, 0.1f, 40 * 48000));              // a long track until the pool is full
    CHECK(r.lp.memoryFull() && r.lp.state(1) == TrackState::Playing && r.lp.trackLength(1) == 20 * 48000);   // rounded down
}
static void loopsWinOverUndo() {
    Rig r(48000.0, 128, size_t(8) << 20); r.lp.setBudgetMb(16);
    recordBase(r, 10 * 48000, 0.1f);
    for (int k = 0; k < 2; ++k) { r.lp.pressRec(0); r.feed(sine(330, 0.05f, 10 * 48000)); r.lp.pressRec(0); r.silence(4800); }
    const uint32_t depth = r.lp.undoDepth(0);
    r.lp.pressRec(1); r.feed(sine(440, 0.1f, 25 * 48000)); r.lp.pressRec(1);   // needs blocks the undo layers hold
    CHECK(r.lp.state(1) == TrackState::Playing && r.lp.trackLength(1) == 30 * 48000);
    CHECK(r.lp.undoDepth(0) < depth && r.lp.layerCount(0) == 3);         // given up, still heard
}
```

- [ ] **Step 2: Run `make test`.** Expected: `test_memory.cpp` fails to compile (`blocksUsed`, `memoryFull`, the 3-argument `init`).
- [ ] **Step 3: Implement.** Pool refcounts and kinds; `TrackBuf`; `Track::buf` becomes a `TrackBuf`, and each of the ~20 `tr.buf` uses goes through `get`/`at`/`zero`. A refused `at()` first gives up the oldest undo layer (the existing `evict()`) and retries; if nothing is left to give up, it sets `memoryFull_` and the write path ends the take: the first recording closes as at `maxRec_`, a recording onto the loop closes as a press, an overdub punches out. The pool is reserved per the Global Constraints, halving on failure down to one full-length track plus 16 MB. Cache the current block per playhead if the bench shows the lookup costs.
- [ ] **Step 4: Run `make test` and `make test-lv2`.** Expected: `==> all tests passed` and `OK`, every existing test unchanged.
- [ ] **Step 5: Measure CPU.** Copy the bench to `tests/bench/bench.cpp`. Build it against `main`'s engine and this branch's (`g++ -O2 -std=gnu++14 -Iplugins/Ouroboros tests/bench/bench.cpp`), best of 3 runs each. Expected: within 3% of `main`, and the printed `out=` value identical. Record both numbers in the task report.
- [ ] **Step 6: Commit.** `git commit -m "engine: track audio in the block pool (allocated on first write, shared copies)"`

---

### Task 2: Parts in the engine

**Files:**
- Modify: `plugins/Ouroboros/dsp/Constants.hpp` (`kParts`), `plugins/Ouroboros/dsp/Looper.hpp`
- Create: `tests/test_parts.cpp`

**Interfaces:**
- Consumes: Task 1's `TrackBuf::shareFrom`, `blocksUsed()`.
- Produces, `Looper`:
  - `void setPart(int p);` the user asks for part p (0-3): queued to the current part's end; stopped: at once; during the first recording: after it closes, then the part's end; p == part(): cancels a queued switch. Task 3 adds: it turns the song off.
  - `int part() const;` `int queuedPart() const;` (-1: none) `uint32_t partLength() const;` (the current part's longest track; L if it has no tracks; 0 before L).
  - Every existing per-track call (`pressRec`, `undo`, `state`, `isOn`, `volumeDb`, ...) acts on track t of the current part.

- [ ] **Step 1: Write the failing tests** in `tests/test_parts.cpp` (free-time, no sync, unless noted):

```cpp
static void switchWaitsForThePartEnd() {
    Rig r; const auto a = recordBase(r, 24000, 0.1f);
    r.silence(6000); r.lp.setPart(1);
    CHECK(r.lp.part() == 0 && r.lp.queuedPart() == 1);
    r.silence(24000 - r.lp.position() - 1); CHECK(r.lp.part() == 0);
    r.silence(2); CHECK(r.lp.part() == 1 && r.lp.queuedPart() == -1);
    CHECK(maxAbsDiff(r.settle(), a) < 1e-6f);                            // the copy plays what A played
}
static void copyIsIndependentAndKeepsOnAndVolume() {
    Rig r; const auto a = recordBase(r, 24000, 0.1f);
    recordBaseOn(r, 1); r.lp.setOn(1, false); r.lp.setVolumeDb(0, -6.0f);
    r.lp.pressRec(0); r.feed(sine(330, 0.05f, 24000)); r.lp.pressRec(0); r.silence(4800);   // A: 2 layers on track 0
    r.lp.setPart(1); r.silence(24000);
    CHECK(r.lp.part() == 1 && !r.lp.isOn(1) && r.lp.volumeDb(0) == -6.0f);
    CHECK(r.lp.layerCount(0) == 1 && r.lp.canUndo(0) && !r.lp.canRedo(0));
    r.lp.pressRec(0); r.feed(sine(660, 0.05f, 24000)); r.lp.pressRec(0); r.silence(4800);   // B changes
    const auto b = r.settle();
    r.lp.setPart(0); r.silence(24000);
    CHECK(r.lp.layerCount(0) == 2 && maxAbsDiff(r.settle(), b) > 0.01f);   // A did not get B's overdub
}
static void copyOnEntryAllocatesNothing() {
    Rig r; recordBase(r, 24000, 0.1f); recordBaseOn(r, 1);
    const uint32_t used = r.lp.blocksUsed();
    r.lp.setPart(1); r.silence(24000);
    CHECK(r.lp.part() == 1 && r.lp.blocksUsed() == used);
}
static void partLengthIsTheLongestTrack() {
    Rig r; recordBase(r, 24000, 0.1f);
    r.lp.pressRec(1); r.feed(sine(330, 0.1f, 3 * 24000 - r.lp.position())); r.lp.pressRec(1);   // a 3-loop track
    CHECK(r.lp.partLength() == 3 * 24000);
    r.lp.setPart(1);
    const uint32_t left = 3 * 24000 - r.lp.trackPosition(1);
    r.silence(left - 1); CHECK(r.lp.part() == 0);
    r.silence(2); CHECK(r.lp.part() == 1);
}
static void choosingTheCurrentPartCancels() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.setPart(2); r.lp.setPart(0);
    CHECK(r.lp.queuedPart() == -1); r.silence(48000); CHECK(r.lp.part() == 0);
}
static void stoppedSwitchesAtOnce() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.stop(ouro::StopMode::Instant); r.silence(2000);
    r.lp.setPart(1); CHECK(r.lp.part() == 1 && r.lp.state(0) == TrackState::Playing);   // filled on entry
}
static void firstRecordingDefersTheSwitch() {
    Rig r; r.lp.pressRec(0); r.feed(sine(220, 0.1f, 12000)); r.lp.setPart(1);
    CHECK(r.lp.part() == 0 && r.lp.queuedPart() == 1);
    r.feed(sine(220, 0.1f, 12000)); r.lp.pressRec(0);                      // L = 24000
    r.silence(23000); CHECK(r.lp.part() == 0);
    r.silence(2000); CHECK(r.lp.part() == 1);
}
static void aRunningTakeClosesAndTheCopyHasItsPunchOut() {
    Rig r; recordBase(r, 24000, 0.1f); r.silence(24000 - r.lp.position() + 6000);
    r.lp.pressRec(0); r.lp.setPart(1);                                    // overdubbing when the part ends
    r.feed(sine(330, 0.05f, 24000));
    CHECK(r.lp.part() == 1 && r.lp.state(0) == TrackState::Playing && r.lp.layerCount(0) == 1);
    const auto b = r.settle();
    r.lp.setPart(0); r.silence(24000);
    CHECK(r.lp.layerCount(0) == 2 && maxAbsDiff(r.settle(), b) < 1e-6f);   // same audio, punch-out fade included
}
static void noClickAtTheSwitch() {
    Rig r; recordBase(r, 24000, 0.2f);
    r.lp.setPart(1); r.silence(24000); r.lp.clearTrack(0); r.silence(4800);
    recordBaseOn(r, 0, 0.2f, 880.0);                                     // B: a different tone
    for (int k = 0; k < 4; ++k) { r.lp.setPart(k % 2 ? 1 : 0); r.silence(24000); }
    CHECK(maxStep(slice(r.out, r.out.size() - 4 * 24000, r.out.size())) <= 1.5f * sineStep(880.0, 0.2f));
}
static void undoOnACopiedTrack() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.setPart(1); r.silence(24000);
    r.lp.undo(0); r.silence(4800); CHECK(r.lp.state(0) == TrackState::Empty);
    r.lp.redo(0); r.silence(4800); CHECK(r.lp.state(0) == TrackState::Playing);
}
static void clearedPartRefillsOnEntry() {
    Rig r; const auto a = recordBase(r, 24000, 0.1f);
    r.lp.setPart(1); r.silence(24000); r.lp.clearTrack(0); r.silence(4800);
    CHECK(r.lp.loopLength() == 24000);                                    // A still holds a track: L stays
    r.lp.setPart(0); r.silence(24000); r.lp.setPart(1); r.silence(24000);
    CHECK(r.lp.part() == 1 && maxAbsDiff(r.settle(), a) < 1e-6f);
}
static void lResetsOnlyWhenEveryPartIsEmpty() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.setPart(1); r.silence(24000);
    r.lp.clearTrack(0); r.silence(4800); CHECK(r.lp.loopLength() == 24000);
    r.lp.setPart(0); r.silence(24000); r.lp.clearTrack(0); r.silence(4800);
    CHECK(r.lp.loopLength() == 0);
}
static void clearAllEmptiesEveryPartAndGoesToA() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.setPart(2); r.silence(24000);
    r.lp.clearAll(); r.silence(4800);
    CHECK(r.lp.part() == 0 && r.lp.loopLength() == 0 && r.lp.blocksUsed() == 0);
}
static void anEmptyPartLastsOneLoop() {
    Rig r; recordBase(r, 24000, 0.1f); r.lp.setPart(1); r.silence(24000);
    r.lp.setPart(0); r.silence(24000); r.lp.clearTrack(0); r.silence(4800);   // A: no tracks, B: one
    r.lp.setPart(1); r.silence(24000 + 128); CHECK(r.lp.part() == 1);
}
static void modStartKeepsAQueuedSwitch() {                               // Sync on, MOD playing (see test_sync's SyncRig)
    SyncRig r; r.lp.setSync(true); r.lp.setTailMs(0); r.toPhase(0);
    r.lp.pressRec(0); r.feedSync(sine(220, 0.1f, 96000)); r.lp.pressRec(0);
    r.silenceSync(30000); r.lp.setPart(1);
    r.lp.modStarted(true);                                               // MOD restarts on a line
    r.silenceSync(1000); CHECK(r.lp.part() == 0 && r.lp.queuedPart() == 1);
    r.silenceSync(96000); CHECK(r.lp.part() == 1);
}
```

  Move `SyncRig` from `test_sync.cpp` into `looper_rig.hpp` so both files use it.

- [ ] **Step 2: Run `make test`.** Expected: `test_parts.cpp` fails to compile (`setPart`, `part`, `queuedPart`, `partLength`).
- [ ] **Step 3: Implement.** Tracks become `parts_[kParts][kTracks]` with a current part; the per-sample loops cover the current part only. The switch happens on the sample the current part's longest track ends its pass. Then:
  - an empty target is filled by `shareFrom` plus the source's on/off, volume, length, phase and pending ops, as one layer each;
  - the new part's tracks start from their beginning, with the start handling a restart uses;
  - the outgoing part goes on being processed (fade-out ramp, punch-out fades, pending writes) until its writers end, then freezes with its playheads at 0. Its last writes must reach the copy too (`aRunningTakeClosesAndTheCopyHasItsPunchOut`).

  A take running in the outgoing part closes at the switch sample, as a press there would. A waiting press or arm on that part is cancelled. `clearAll()` returns to part A. L resets only when all 16 tracks are empty.
- [ ] **Step 4: Run `make test`.** Expected: `==> all tests passed`.
- [ ] **Step 5: Commit.** `git commit -m "engine: four parts of four tracks, switched at the part's end, filled on entry"`

---

### Task 3: The song in the engine

**Files:**
- Modify: `plugins/Ouroboros/dsp/Constants.hpp` (`kSteps`, `kStepLoop = 4`, `kStepStop = 5`), `plugins/Ouroboros/dsp/Looper.hpp`
- Create: `tests/test_song.cpp`

**Interfaces:**
- Consumes: Task 2's parts and switch.
- Produces, `Looper`:
  - `void setSong(bool on);`
  - `void setStep(int i, int part, uint32_t repeats);` i 0-15; part 0-3, `kStepLoop` or `kStepStop`; repeats 1-16. Default: step 0 = part 0 x1, every other step = `kStepLoop` x1.
  - `bool songOn() const;` `int songStep() const;` (the step playing, -1 when not playing) `uint32_t songLength() const;` (steps before the first Loop/Stop) `uint32_t songPass() const;` (passes of the current step completed)
  - `int stepPart(int i) const;` `uint32_t stepRepeats(int i) const;` (the arrangement, for the plugin and the song widget)
  - `setPart()` turns the song off (the song's own switches don't go through it).

- [ ] **Step 1: Write the failing tests** in `tests/test_song.cpp`. Helper: `twoParts(Rig&)` records a 24000-sample track in A, switches to B, clears B's track 0 and records an 880 Hz one, switches back to A, then feeds exactly up to A's next part end, so A plays at position 0. The checks land 64 samples past a part end, never on it.

```cpp
static void songPlaysItsStepsInOrder() {
    Rig r; twoParts(r);
    r.lp.setStep(0, 0, 2); r.lp.setStep(1, 1, 1); r.lp.setStep(2, ouro::kStepLoop, 1);
    r.lp.setSong(true); CHECK(r.lp.songOn() && r.lp.songStep() == -1);
    r.silence(24000 + 64); CHECK(r.lp.songStep() == 0 && r.lp.part() == 0);   // starts at the next part end
    r.silence(2 * 24000); CHECK(r.lp.songStep() == 1 && r.lp.part() == 1);
    r.silence(24000); CHECK(r.lp.songStep() == 0 && r.lp.part() == 0);        // Loop
    CHECK(r.lp.songLength() == 2);
}
static void aStopStepStopsAndRewinds() {
    Rig r; twoParts(r); r.lp.setStep(0, 1, 1); r.lp.setStep(1, ouro::kStepStop, 1);
    r.lp.setSong(true); r.silence(24000 + 24000 + 128);
    CHECK(r.lp.transport() == ouro::Transport::Stopped && r.lp.songOn() && r.lp.songStep() == -1);
    r.lp.pressRec(0);                                                    // B from stopped: a restart
    CHECK(r.lp.transport() == ouro::Transport::Running && r.lp.songStep() == 0 && r.lp.part() == 1);
}
static void songFromNothingCountsFromTheFirstClose() {
    Rig r; r.lp.setStep(0, 0, 2); r.lp.setStep(1, 1, 1); r.lp.setSong(true);
    r.lp.pressRec(0); r.feed(sine(220, 0.1f, 24000)); r.lp.pressRec(0);   // closes at position 0
    CHECK(r.lp.songStep() == 0 && r.lp.songPass() == 0);
    r.silence(2 * 24000 + 64);
    CHECK(r.lp.part() == 1 && r.lp.state(0) == TrackState::Playing);    // B filled from A on entry
}
static void stepsAfterTheFirstLoopAreNotPlayed() {
    Rig r; twoParts(r); r.lp.setStep(0, 0, 1); r.lp.setStep(1, ouro::kStepLoop, 1); r.lp.setStep(2, 1, 1);
    r.lp.setSong(true); r.silence(5 * 24000); CHECK(r.lp.part() == 0 && r.lp.songLength() == 1);
}
static void aHandOnPartTurnsTheSongOff() {
    Rig r; twoParts(r); r.lp.setSong(true); r.silence(24000);
    r.lp.setPart(1); CHECK(!r.lp.songOn() && r.lp.queuedPart() == 1 && r.lp.songStep() == -1);
}
static void repeatsCountPassesOfTheWholePart() {
    Rig r; twoParts(r);
    r.lp.pressRec(1); r.feed(sine(330, 0.1f, 2 * 24000)); r.lp.pressRec(1);   // A gets a 2-loop track
    r.silence(2 * 24000 - r.lp.trackPosition(1));                         // A's top
    r.lp.setStep(0, 0, 2); r.lp.setStep(1, 1, 1); r.lp.setSong(true);
    r.silence(2 * 24000 + 64); CHECK(r.lp.songStep() == 0);              // the song starts at A's end
    r.silence(4 * 24000 - 128); CHECK(r.lp.part() == 0);                 // two passes of 2 loops
    r.silence(128); CHECK(r.lp.part() == 1);
}
static void clearAllRewindsTheSong() {
    Rig r; twoParts(r); r.lp.setSong(true); r.silence(2 * 24000);
    r.lp.clearAll(); r.silence(4800);
    CHECK(r.lp.songOn() && r.lp.songStep() == -1 && r.lp.part() == 0);
}
static void theDefaultSongRepeatsA() {
    Rig r; twoParts(r); r.lp.setSong(true); r.silence(5 * 24000);
    CHECK(r.lp.part() == 0 && r.lp.songStep() == 0 && r.lp.songLength() == 1);
}
```

- [ ] **Step 2: Run `make test`.** Expected: `test_song.cpp` fails to compile.
- [ ] **Step 3: Implement.**
  - **Advancing:** at each part end while the song plays, count the pass and, once the step's repeats are done, go to the next step: switch to its part (copy on entry applies), loop to step 0 on `kStepLoop`, or stop on `kStepStop`. A stop uses the current Stop Mode, then the song rewinds to not playing with Song still on.
  - **Starting:** turning Song on while playing starts step 0 at the next part end. Any restart while Song is on starts step 0 from the top (`restart()`, a MOD start, Stop/Play). With nothing recorded, the first loop is recorded in step 0's part, and counting starts at its close.
  - **Clearing:** `clearAll()` rewinds the song.
- [ ] **Step 4: Run `make test`.** Expected: `==> all tests passed`.
- [ ] **Step 5: Commit.** `git commit -m "engine: the song (16 steps of part x repeats, Loop and Stop) advanced at part ends"`

---

### Task 4: Plugin controls and saved arrangement

**Files:**
- Modify: `plugins/Ouroboros/OuroborosPlugin.hpp`
- Test: `tests/test_plugin.cpp`, `tests/lv2/test_lv2_hooks.cpp` (`gPorts` grows to 128 entries; `rec` stays at LV2 index 4)

**Interfaces:**
- Consumes: Tasks 2 and 3.
- Produces, the new parameters, in this order before `kParamBypass`:
  - `kParamPart` (symbol `part`, Enum "A".."D", default 0)
  - `kParamSong` (`song`, Bool, default 0)
  - `kParamStep` (`step`, Int 1-16, default 1)
  - `kParamStepPart` (`step_part`, Enum "A","B","C","D","Loop","Stop", default 0)
  - `kParamStepRepeats` (`step_repeats`, Int 1-16, default 1)
  - `kParamStep1Part` .. `kParamStep16Repeats`, interleaved (`step1_part`, `step1_repeats`, ..., `step16_part`, `step16_repeats`), `kParameterIsHidden`. Part defaults: step 1 = 0 (A), others = 4 (Loop). Repeats default 1.
  - New port group `kGroupSong` (`g4_song`, "Song") for all of them.

- [ ] **Step 1: Write the failing tests** in `tests/test_plugin.cpp`:
  - `layout`: 34 old symbols unchanged, then `part`, `song`, `step`, `step_part`, `step_repeats`, `step1_part`, `step1_repeats` ... `step16_repeats`. `kParamBypass == 71`, count 72. The step ports carry `kParameterIsHidden`.
  - `partPortSwitchesAtThePartEnd`: record a loop; `set(kParamPart, 1)` → `eng().queuedPart() == 1`; after the part end `eng().part() == 1`.
  - `knobFollowsTheSong`: arrangement A x1, B x1, Loop through the hidden ports, `set(kParamSong, 1)`. When the engine switches, `hasReq(kParamPart, 1)`.
  - `handOnPartTurnsSongPortOff`: song playing, `set(kParamPart, 0)` → `hasReq(kParamSong, 0)` and `!eng().songOn()`.
  - `stepProxyEditsTheSelectedStep`: `set(kParamStep, 3)` → requests `kParamStepPart`/`kParamStepRepeats` with step 3's values. `set(kParamStepRepeats, 4)` → `hasReq(kParamStep3Repeats, 4)`, and the engine has step index 2 at 4 repeats.
  - `hiddenStepPortsRestoreTheArrangement`: set the hidden ports before the first run (as a pedalboard load does); after `runN(1)` the engine's song plays them (check via a song run).
  - `onAndVolumePortsFollowThePart`: in A, track 1 off; switch to B (filled), turn track 1 on there; back in A → `hasReq(kParamOn2, 0)`, in B → `hasReq(kParamOn2, 1)`.
  - `loadWithSongOnStartsInStepOnesPart`: hidden step 1 = C, `kParamSong` = 1 before the first run → `eng().part() == 2` and `hasReq(kParamPart, 2)`.
  - `clearAllRequestsPartA`.
- [ ] **Step 2: Run `make test`.** Expected: the new checks fail (unknown parameters).
- [ ] **Step 3: Implement.**
  - **Part and Song:** Part and Song changes from the host go to `setPart`/`setSong`. After `process()`, when `eng().part()` or `eng().songOn()` differs from the port, `request()` the port (as the Volume proxy does), so the plugin's own requests don't count as user changes.
  - **Step proxies:** Step selects a step. Step Part and Step Repeats show and edit it, writing the hidden port and `setStep`. Hidden-port changes, including every one on the first run, go to `setStep`.
  - **The current part on the ports:** On 1-4, Volume 1-4 and the Volume proxy are re-requested from the current part after a switch.
- [ ] **Step 4: Run `make test` and `make test-lv2`.** Expected: `==> all tests passed` and `OK`.
- [ ] **Step 5: Commit.** `git commit -m "plugin: Part, Song and the song screen's controls; the arrangement in hidden ports"`

---

### Task 5: Widgets

**Files:**
- Modify: `plugins/Ouroboros/hmi/LoopWidget.hpp`, `plugins/Ouroboros/hmi.ttl.in`, `plugins/Ouroboros/OuroborosPlugin.hpp` (`widgetShown` accepts `song`; each slot remembers its symbol)
- Create: `plugins/Ouroboros/hmi/SongWidget.hpp`
- Test: `tests/test_widget.cpp`, `tests/lv2/test_lv2_hooks.cpp`

**Interfaces:**
- Consumes: `part()`, `queuedPart()`, `songStep()`, `songLength()`, `memoryFull()`, `stepPart(i)`, `stepRepeats(i)` (Tasks 1-3).
- Produces:
  - `WidgetView` gains `int part, queuedPart, songStep, songLength; bool memoryFull;`
  - `struct SongView { uint8_t part[kSteps]; uint8_t repeats[kSteps]; int playing; int edited; bool on; };` and `size_t renderSongWidget(const SongView& v, uint16_t w, uint16_t h, uint8_t* bits);` (Mono1, as `renderLoopWidget`)
  - `hmi.ttl.in`: a second `ahw:widget`, `lv2:symbol "song"`, `lv2:name "Song"`, natural 128 x 37, minimum 96 x 16.

- [ ] **Step 1: Write the failing tests** in `tests/test_widget.cpp`:
  - The loop widget at 40 x 37:
    - rendering with `part = 1` differs from `part = 0`;
    - `queuedPart = 2` differs from `queuedPart = -1`;
    - `songStep = 2, songLength = 6` differs from `songStep = -1`;
    - `memoryFull` differs from not.
    - The existing layout tests still pass, so nothing moved that they pin.
  - The song widget at 128 x 37: each of steps 0-5's part letter is drawn (a glyph check on that column, using the test file's existing glyph helpers); the playing step and the edited step are marked; steps after the first Loop/Stop show nothing; 96 x 16 renders without writing past the buffer.
  - `test_lv2_hooks`: `widget_shown(h, "song", ...)` then 40 runs → a draw arrives.
- [ ] **Step 2: Run `make test` and `make test-lv2`.** Expected: the new checks fail.
- [ ] **Step 3: Implement.** The layouts follow the spec's sketches. The part letter sits beside the track number; `A>B` blinks with `blinkOn`; `FULL` takes the state text's place while memory is full.
- [ ] **Step 4: Run both suites.** Expected: green.
- [ ] **Step 5: Commit.** `git commit -m "widgets: the part, a queued switch, the song position and FULL; the song widget"`

---

### Task 6: Manual, README, CPU on the Dwarf

**Files:**
- Modify: `docs/manual/ouroboros-manual.html`, `docs/manual/ouroboros-manual.pdf`, `README.md`

- [ ] **Step 1: Manual.** Add a **Song parts** section covering:
  - parts and copy on entry;
  - switching at the part's end;
  - the song (steps, Loop and Stop, starting, a hand on Part);
  - the subpage 2 and 3 controls, with the Starless widget placement for "Song";
  - memory full.

  Also add the new rows to the settings table. Run `make manual`. Expected: at most 3 pages and nothing cut off. Render the pages with `pdftoppm` and look at every page's bottom.
- [ ] **Step 2: README.** One feature line for parts and the song.
- [ ] **Step 3: CPU on the Dwarf.** Build the bench for aarch64 against `main` and this branch, the same way as `tests/bench/bench.cpp` in Task 1 (cross g++ in the `moddwarf-cross` image, `-static-libstdc++`). Copy both to the Dwarf with `scp -O` to `/tmp`, run each twice, then delete them. Expected: the branch is within noise of `main`. Record the numbers in the task report. If the Dwarf is unreachable, record that, and the check stays open for the user.
- [ ] **Step 4: Commit.** `git commit -m "docs: song parts (manual, README)"`
