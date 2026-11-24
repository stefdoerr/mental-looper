# Saved Pieces Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Save the whole piece (4 parts × 4 tracks, mix, loop length, arrangement) to one of 16 numbered slots on the Dwarf, and load any slot back between songs.

**Architecture:** The engine gains a snapshot (the tracks' blocks shared into a hidden 5th set of tracks, pending undo/redo resolved on read) and an import path. A plugin-owned worker thread writes and reads the slot folders: for a save it only reads the snapshot while the audio keeps playing; for a load the audio thread lends it the whole engine once the loops are stopped. The plugin's run() drives Save/Load presses, confirmations, statuses and the remembered slot; a new widget shows the slots.

**Tech Stack:** C++14 (gnu++14), DPF (LV2), POSIX file I/O, DPF `Thread`, the project's own test harness (`make test`, `make test-lv2`).

**Spec:** `docs/superpowers/specs/2026-11-24-ouroboros-saved-pieces-design.md`

## Global Constraints

- Real-time safe: no file I/O, allocation, locks, syscalls or unbounded work in `run()`/`process()`. All file work runs on the worker thread.
- One owner for the engine at a time: the audio thread; the worker only reads the snapshot (save), or owns the whole engine while it is lent (load), during which `run()` makes no `fEngine` call.
- CPU rule: playing costs what main costs. No per-sample work is added; the bench's `out=` stays identical; checked on x86 and on the Dwarf at the plugin's real flags (`-O3 -ffast-math` plus the MOD toolchain wrapper's flags), pinned to one core, interleaved runs against main.
- Existing parameter symbols are frozen; new controls go before Bypass: `slot`, `save`, `load`, `piece` (hidden). `kParamBypass` becomes 75, count 76.
- Slots 1–16, folder `<user files>/Audio Loops/Ouroboros/01` … `16`; one mono 32-bit float WAV per non-empty track named `A1.wav` … `D4.wav`; `piece.txt` in format version 1 (Task 1).
- Confirmation window 2 s; transient statuses show for 3 s.
- Any DPF change stays in the local patch; never proposed upstream.
- Every commit message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. An overdub, Clear All, Undo or a part switch while a save writes: the saved piece is the piece at the snapshot, exactly (Task 2 test `snapshotIgnoresLaterChanges`).
2. The pool filling while a snapshot is held: the save is cancelled, the slot keeps its old piece, the audio behaves as with any full pool (Task 3 `refusalCancelsTheSave`).
3. A pedalboard whose remembered slot is empty, broken or from another rate: it comes up with no piece and the status says why (Task 4 `autoLoadOfABrokenSlotShowsWhy`).
4. Power loss mid-save: the next start recovers the slot from `NN.old` and drops `NN.new` (Task 1 `recoverSlotsAfterACrash`).
5. A piece needing more blocks than the pool (shared audio flattened): `TOO BIG` before anything is cleared (Task 3 `tooBigLeavesTheEngine`).

---

### Task 1: Piece files (format, WAV, slot folders)

**Files:**
- Create: `plugins/Ouroboros/dsp/Piece.hpp` (the data a piece holds; no I/O)
- Create: `plugins/Ouroboros/pieces/PieceFile.hpp` (WAV, `piece.txt`, slot folders)
- Test: `tests/test_piecefile.cpp`

**Interfaces:**
- Produces (`dsp/Piece.hpp`, namespace `ouro`):
  ```cpp
  struct PieceTrack { uint32_t len = 0; float volDb = 0.0f; bool on = true; };
  struct PieceMeta {
      uint32_t rate = 0, loop = 0, bars = 1;     // sample rate, L, the first loop's bars
      float bpm = 0.0f;                          // 0: unknown
      int part = 0;                              // 0-3
      bool song = false;
      int8_t stepPart[kSteps];                   // as Looper::setStep: 0-3, kStepLoop, kStepStop (default A, then Loop)
      uint8_t stepReps[kSteps];                  // 1-16 (default 1)
      PieceTrack track[kParts][kTracks];
      PieceMeta();                               // fills the step defaults
  };
  uint32_t blocksFor(const PieceMeta& m);        // sum over tracks of ceil(len / BlockPool::kBlock)
  ```
- Produces (`pieces/PieceFile.hpp`, namespace `ouro { namespace pieces {`):
  ```cpp
  enum class Err { None, NoSpace, Io };          // NoSpace: errno ENOSPC or EDQUOT
  std::string piecesDir();                       // $MOD_USER_FILES_DIR, else /data/user-files if a directory, else $HOME/.ouroboros;
                                                 // + "/Audio Loops/Ouroboros". "" on _WIN32 (pieces unavailable)
  std::string slotDir(const std::string& root, int slot);    // root + "/01" .. "/16"
  std::string trackName(int part, int track);                // "A1" .. "D4"
  class WavWriter { public: bool open(const std::string& path, uint32_t rate);
                    Err write(const float* s, uint32_t n); Err close(); };   // close() patches the sizes and fsyncs
  class WavReader { public: bool open(const std::string& path);              // RIFF/WAVE, format 3, 1 channel, 32 bit
                    uint32_t rate() const; uint32_t frames() const; bool read(float* s, uint32_t n); };
  std::string formatPiece(const PieceMeta& m);
  bool parsePiece(const std::string& text, PieceMeta& m);   // false on any deviation from version 1
  void recoverSlots(const std::string& root);    // per slot: drop NN.new; NN missing and NN.old present -> rename back; both -> drop NN.old
  uint32_t scanSlots(const std::string& root);   // bit s-1 set: slot s has NN/piece.txt
  std::string stagingDir(const std::string& root, int slot);  // root + "/NN.new", created empty (parents too)
  Err commitSlot(const std::string& root, int slot);          // NN -> NN.old (if present), NN.new -> NN, drop NN.old; fsync root
  void removeTree(const std::string& dir);
  ```
- `piece.txt` version 1, exactly these lines in this order (`%.2f` for dB, `%.3f` for BPM, `on` is 0/1; 16 `track` lines A1..A4, B1..B4, C1..C4, D1..D4, all present, `len` 0 for empty tracks):
  ```
  ouroboros-piece 1
  rate 48000
  loop 96000
  bars 4
  bpm 120.000
  part 1
  song 1
  steps 0:2 1:1 0:2 4:1 4:1 4:1 4:1 4:1 4:1 4:1 4:1 4:1 4:1 4:1 4:1 4:1
  track A1 192000 -3.50 1
  ```
  `parsePiece` also rejects: `loop` 0 with any non-empty track, a non-empty `len` that is not a multiple of `loop`, `part` outside 0–3, a step outside its range.

- [ ] **Step 1: Write the failing tests** in `tests/test_piecefile.cpp` (temp root from `mkdtemp("build/tests/pfXXXXXX")`):
  - `wavRoundTripIsExact`: write 10000 samples of `sine(220, 0.3f, 10000)` plus `-0.0f`, `1e-30f` at 48000; read back bit-identical; `rate() == 48000`, `frames() == 10002`.
  - `wavReaderRejectsOtherFormats`: a 16-bit PCM header, a 2-channel float header, a truncated data chunk → `open()` false.
  - `wavReaderSkipsUnknownChunks`: a `LIST` chunk between `fmt ` and `data` → reads.
  - `pieceTextRoundTrip`: a meta with rate 48000, loop 96000, bars 4, bpm 120, part 1, song on, steps `0:2 1:1 0:2` then Loop, A1 len 192000 −3.5 dB on, B3 len 96000 off → `parsePiece(formatPiece(m))` equal field by field; `formatPiece` of that meta equals the version-1 sample text above for its first 9 lines.
  - `parsePieceRejects`: version 2, a missing line, len 100000 with loop 96000, part 4, step part 6, reps 0 → false.
  - `recoverSlotsAfterACrash`: (a) only `03.old` → becomes `03`; (b) `03` and `03.old` → `03.old` gone; (c) `05.new` → gone; `scanSlots` then reports slot 3 only.
  - `commitReplacesAtomically`: slot 2 holds piece X; stage piece Y; `commitSlot` → slot 2 holds Y, no `02.old`/`02.new` left.
  - `blocksForCountsWholeBlocks`: lens 4096, 4097, 0 → 1 + 2 + 0 = 3.
- [ ] **Step 2: Run, expect a compile failure** — `make test` → `test_piecefile.cpp`: missing `dsp/Piece.hpp`.
- [ ] **Step 3: Implement** `dsp/Piece.hpp` and `pieces/PieceFile.hpp`. POSIX only (`open`/`write`/`fsync`/`rename`/`mkdir`/`opendir`); `#ifdef _WIN32` every function returns an error/empty value so the Windows build compiles.
- [ ] **Step 4: Run** `make test` → `==> all tests passed`.
- [ ] **Step 5: Commit** `pieces: the piece format, WAV files and slot folders`.

### Task 2: Engine snapshot, import and change count

**Files:**
- Modify: `plugins/Ouroboros/dsp/BlockPool.hpp`, `plugins/Ouroboros/dsp/Looper.hpp`
- Test: `tests/test_pieces.cpp`

**Interfaces:**
- Consumes: `PieceMeta` (Task 1).
- Produces (`BlockPool`): `uint32_t reserved() const;` `uint32_t refusals() const;` (incremented on each refused Audio block, in `alloc()`'s refusal branch only).
- Produces (`Looper`, all audio-thread unless noted):
  ```cpp
  bool settled() const;          // no take (recording, overdub, first recording), tail, punch-out fade in any part, no outgoing part
  bool takeSnapshot(PieceMeta& m);   // settled() and none held: shares every track into the snapshot (copy()), fills m except bpm
  bool snapshotHeld() const;
  // Worker thread, while held: n samples of track (p, t) from sample `from` of its beginning (index (len - phase) % len),
  // as heard once every pending undo/redo op has passed (fold ops too). Reads the snapshot only; local DeltaCursors.
  void readSnapshot(int p, int t, uint32_t from, float* dst, uint32_t n) const;
  void dropSnapshot();
  // Worker thread, engine lent (no process() or control call meanwhile):
  void importBegin();            // reset(): every part empty, L unset; transport Stopped
  bool importTrack(int p, int t, uint32_t from, const float* src, uint32_t n);   // false: pool refused
  void importEnd(const PieceMeta& m);   // L, bars, each track's len (Playing, pos 0, phase 0), volume, on; steps, song, part;
                                        // stopped at the top: restart() plays every track from its beginning
  uint32_t capacityBlocks() const;   // pool_.reserved()
  uint32_t maxTrackLength() const;   // maxRec_
  uint32_t audioRefusals() const;    // pool_.refusals()
  uint32_t changes() const;      // +1 on: a take's close, an accepted undo/redo, clearTrack, clearAll, setOn/toggleOn and
                                 // setVolumeDb and setStep and setSong when the value changes, a copy on entry. Not on part
                                 // switches without a copy, playback or the song advancing.
  bool empty() const;            // allEmpty()
  ```
- The snapshot is `Part snap_[kParts]`: buffers `init(cap_)` in `init()`, dropped in `release()`; untouched by `reset()`, `clearAll()` and switches.

- [ ] **Step 1: Write the failing tests** in `tests/test_pieces.cpp` with `Rig`:
  - `snapshotReadsFromTheTrackBeginning`: record a base on track 0 (`recordBase(r, 24000, 0.2f)`), then a 2 L track on track 1 recorded starting mid-loop (phase ≠ 0); `takeSnapshot`; `readSnapshot` of each equals one pass captured after `restart()` from stopped (stop Instant, settle, restart, capture `len`), sample for sample (tolerance 1e-6).
  - `snapshotResolvesPendingOps`: base + overdub on track 0, `undo(0)`, `takeSnapshot` at once (op pending, `pendingOps(0) == 1`); `readSnapshot` equals `r.settle()`'s image after the undo pass.
  - `snapshotIgnoresLaterChanges`: snapshot, then overdub track 0, `undo` on track 1, `clearAll()`; `readSnapshot` unchanged; `dropSnapshot()` → `blocksUsed()` back to what the live tracks hold.
  - `takeSnapshotWaitsForASettledEngine`: false while recording, while overdubbing, during the first loop's tail, and with one held; true after.
  - `snapshotMetaHoldsTheMix`: volumes −6 dB on track 2 of part B, track 3 off, steps `1:2`, song on → `m` matches; `m.track[p][t].len == trackLength`; `m.loop == loopLength()`.
  - `importPlaysBackExactly`: snapshot a piece in rig 1, import it into rig 2 (`importBegin`, `importTrack` in 4096-sample chunks, `importEnd`); rig 2 is `Stopped`, every `trackLength` equal, `restart()` then one pass equals rig 1's pass after its own `restart()` (tolerance 1e-6); `undoDepth` 0 everywhere; part and song as saved.
  - `importedPieceFitsCapacity`: `blocksFor(m) <= capacityBlocks()` for the default pool; a 1 MB pool rig reports `capacityBlocks() == 64`.
  - `changesCountsWhatASaveWrites`: count rises on a close, an undo, `setVolumeDb(0, -3)`, `setStep(1, 1, 2)`; unchanged on `setVolumeDb(0, -3)` again, `setPart` to a non-empty part, and 10 passes of playback.
- [ ] **Step 2: Run, expect a compile failure** (missing `takeSnapshot`).
- [ ] **Step 3: Implement** the listed members. `takeSnapshot` reuses `copy()` per track into `snap_`; `readSnapshot` adds each pending op's contribution at the indices it still owes (`(idx + len - op.at) % len < op.len - op.done`): non-fold `sign * d.read(off)` for `off = (idx + len - d.start()) % len < d.length()`, fold `buf.get(idx + len)`.
- [ ] **Step 4: Run** `make test` → `==> all tests passed`; then the bench (`tests/bench/bench.cpp`, x86, `-O3 -ffast-math`) against main: `out=` identical.
- [ ] **Step 5: Commit** `engine: a snapshot for saving, an import for loading, a change count`.

### Task 3: The worker thread

**Files:**
- Create: `plugins/Ouroboros/pieces/PieceWorker.hpp`
- Test: `tests/test_worker.cpp`

**Interfaces:**
- Consumes: Task 1 files API, Task 2 engine API.
- Produces (`ouro::pieces::PieceWorker : public DISTRHO::Thread`):
  ```cpp
  enum class Result : int { None, Saved, DiskFull, SaveError, Cancelled, Loaded, Empty, TooBig, LoadError };
  PieceWorker(Looper& engine, std::string root);   // non-RT; recoverSlots + scanSlots in run()'s start
  // audio thread (lock-free: atomics, release/acquire):
  bool idle() const;
  void startSave(int slot, const PieceMeta& m, uint32_t refusals);   // snapshot held until the result is taken;
                                                   // refusals: engine.audioRefusals() at the snapshot
  void watch(uint32_t refusals);                   // per block: a save cancels itself once refusals exceeds its start value
  void startLoad(int slot);                        // reads the rate, capacity and max length from the engine now
  bool wantsEngine() const;                        // a load passed every check and asks for the engine
  void lendEngine();                               // from now until poll() returns the load's result: no fEngine call
  void cancel();                                   // a save stops between chunks and drops NN.new
  Result poll();                                   // the finished job's result once, else None
  float progress() const;                          // 0..1 of the load's samples
  const PieceMeta& loaded() const;                 // valid after Loaded
  uint32_t fullSlots() const;                      // scanSlots() as of the last job (bit s-1)
  void halt();                                     // non-RT: cancel, wait until idle (activate, sampleRateChanged, destructor)
  ```
- Load checks before asking for the engine, in this order, each failing the job without touching the engine: no `NN/piece.txt` → `Empty`; `parsePiece` false, `rate` ≠ engine rate, a len > `maxTrackLength()`, a missing WAV or one whose rate/frames differ from `piece.txt` → `LoadError`; `blocksFor(m) > capacityBlocks()` → `TooBig`. A read error after `importBegin` → `importBegin()` again (every part empty), `LoadError`.
- The thread polls for a job every 20 ms (`d_msleep`), and every 1 ms while it waits for the engine; chunks are 4096 samples.

- [ ] **Step 1: Write the failing tests** in `tests/test_worker.cpp` (temp root; a `Rig` as the engine; the test plays the audio thread: it calls `r.lp.process` via `r.silence(128)` and `poll()` in a loop with a 5 s timeout):
  - `saveWritesTheSlot`: record a 2-part piece, `takeSnapshot`, `startSave(3, m)` → `Saved`; `03/piece.txt` parses to `m`; each WAV equals `readSnapshot`; `fullSlots() == 1u << 2`.
  - `saveWhilePlayingChangesNothingHeard`: output during the save equals the same rig's output without one (two rigs, same input).
  - `cancelLeavesTheSlot`: slot 3 holds X; start a save of Y, `cancel()` at once → `Cancelled`, slot 3 still X, no `03.new`.
  - `refusalCancelsTheSave`: a 2 MB pool rig, a piece filling most of it, snapshot, `startSave(3, m, r.lp.audioRefusals())`, then overdub track 0 calling `watch(r.lp.audioRefusals())` every block until a write is refused → `Cancelled`, slot 3 unchanged, `dropSnapshot()` returns the shared blocks.
  - `saveToAReadOnlyRootFails`: root made 0555 → `SaveError`; slot unchanged.
  - `loadRestoresThePiece`: save piece P to slot 4, `clearAll()`, `startLoad(4)`; when `wantsEngine()`, `lendEngine()`; → `Loaded`; restart and compare one pass with P's.
  - `loadChecksBeforeTouching`: an empty slot → `Empty`; a `piece.txt` with rate 44100 → `LoadError`; a missing `B2.wav` → `LoadError`; in each, `wantsEngine()` never true and the engine's tracks unchanged.
  - `tooBigLeavesTheEngine`: save from a default-pool rig a piece whose part B is a copy of A (shared blocks); load it into a rig whose `capacityBlocks()` is below `blocksFor(m)` → `TooBig`, engine unchanged.
- [ ] **Step 2: Run, expect a compile failure.**
- [ ] **Step 3: Implement** `PieceWorker`.
- [ ] **Step 4: Run** `make test` → `==> all tests passed` (ASan clean).
- [ ] **Step 5: Commit** `pieces: the worker thread saves and loads slots`.

### Task 4: Plugin controls and the save/load flow

**Files:**
- Modify: `plugins/Ouroboros/OuroborosPlugin.hpp`
- Test: `tests/test_plugin.cpp`, `tests/lv2/test_lv2_hooks.cpp`

**Interfaces:**
- Consumes: Task 3 `PieceWorker`, Task 2 engine API.
- Produces: params `kParamSlot` (`slot`, "Slot", Int 1–16, def 1), `kParamSave` (`save`, "Save", Trig), `kParamLoad` (`load`, "Load", Trig), `kParamPiece` (`piece`, "Piece", Int 0–16, def 0, hidden; 0 = none), in that order after `kParamStep16Repeats`; group `kGroupPieces` ("Pieces", `g5_pieces`). The step branch of `initParameter` covers only `kParamStep1Part..kParamStep16Repeats`.
- Produces for Task 5: `const char* pieceStatus() const;` (the status line, below) and `bool pieceLoading() const;`.
- Flow (run(), per block):
  - **Save** (rising edge), worker idle, engine not `empty()`: slot not in `fullSlots()` or confirmed → wait for `settled()` (status `SAVING`), then `takeSnapshot`, `m.bpm` = MOD's BPM when Sync is on and the grid valid (else 0), `startSave`. Full slot, unconfirmed → status `SAVE N?` for 2 s; a second Save confirms; a Slot change, a Load or the 2 s passing cancels.
  - **Load** (rising edge), worker idle: slot empty → status `EMPTY` (nothing stops). `changes()` differs from the last save/load and engine not `empty()`, unconfirmed → `LOAD N?`, same confirmation rules. Else `fEngine.stop(Instant)`, `startLoad`; once `wantsEngine()` and `transport() == Stopped`, `lendEngine()`.
  - **From a load's start to its result:** no control or press is applied (`fPrev` takes `fVal`); while lent, run() writes the dry input (Dry Thru on) or silence, a silent Click, and draws no engine state.
  - **Results:** `Saved` → status `SAVED N`, `request(kParamPiece, N)`, baseline = `changes()` at the snapshot; `DiskFull`/`SaveError`/`Cancelled` → `DISK FULL`/`SAVE ERROR`/`SAVE ERROR`. Any save result → `dropSnapshot()`. `Loaded` → push every stored control (Part, Song, the 32 step ports and the Step proxies, On 1–4, Volume 1–4, Volume via the existing part-change path with `fPart = -1`), `request(kParamPiece, N)`, `request(kParamSlot, N)`, baseline = `changes()`, status `120 BPM 4 BARS` (`4 BARS` when bpm is 0) until the next Slot change, Save or Load. `Empty`/`TooBig`/`LoadError` → `EMPTY`/`TOO BIG`/`LOAD ERROR`.
  - **Cancel on memory:** while a save writes, `fWorker.watch(fEngine.audioRefusals())` every block.
  - **While a load runs:** status `LOADING NN%` from `progress()` (`LOADING 0%` during the checks and the stop).
  - **Pedalboard load:** on the first run (`fFirst`), `piece` 1–16 → a load of that slot without confirmation.
  - Transient statuses last 3 s; idle status `N FULL` or `N EMPTY` for the selected slot, with `LOADED M` right-aligned when a piece was loaded or saved (M = `piece`).
  - `activate()`, `sampleRateChanged()` and the destructor call `fWorker.halt()` first, then `dropSnapshot()` if held. The worker starts in the constructor with `pieces::piecesDir()`.

- [ ] **Step 1: Write the failing tests** (in `test_plugin.cpp`, `setenv("MOD_USER_FILES_DIR", tempdir)` before `fresh()`; helper `waitPieces()` runs blocks until the worker is idle and the status settles, 5 s timeout):
  - layout: `kParamBypass == 75 && P().getParameterCount() == 76`; symbols `slot`, `save`, `load`, `piece`; `piece` hidden.
  - `saveAndLoadRoundTrip`: record a base and a dub, Save to slot 2 → status `SAVED 2`, `hasReq(kParamPiece, 2)`; Clear All; Load → `LOAD 2?` not shown (nothing to lose), output after Stop/Play equals the recording's.
  - `saveOnAFullSlotAsksFirst`: second Save to slot 2 shows `SAVE 2?`, a Slot change cancels (no write: file mtime unchanged), Save twice within 2 s writes.
  - `loadWithChangesAsksFirst`: after an overdub, Load shows `LOAD 2?`; 2 s of blocks later it is cancelled; Load twice loads.
  - `pressesDuringALoadAreIgnored`: Rec pressed while loading → after the load no track is Recording and the transport is Stopped.
  - `controlsFollowTheLoadedPiece`: piece with part B, Song on, step 2 = C×3, B1 at −6 dB → after Load the requests set `part` 1, `song` 1, `step2_part` 2, `step2_repeats` 3, `volume_1` −6.
  - `autoLoadOnFirstRun`: fresh plugin with `piece` = 2 before the first run → the piece loads, `hasReq(kParamSlot, 2)`.
  - `autoLoadOfABrokenSlotShowsWhy`: `piece` = 7 (empty) → status `EMPTY`, engine empty; a slot with a corrupt `piece.txt` → `LOAD ERROR`.
  - `saveStoresTheBpm`: Sync on with a 120 BPM time position → the slot's `piece.txt` has `bpm 120.000`; Sync off → `bpm 0.000`.
  - lv2 test: the parameter count and the indices of `slot`/`piece` match `kLv2FirstParam + kParamSlot` and `+ kParamPiece`.
- [ ] **Step 2: Run, expect failures** (unknown params).
- [ ] **Step 3: Implement** the params and the flow.
- [ ] **Step 4: Run** `make test` and `make test-lv2` → both pass.
- [ ] **Step 5: Commit** `plugin: Slot, Save and Load, the remembered piece`.

### Task 5: The pieces widget

**Files:**
- Create: `plugins/Ouroboros/hmi/PiecesWidget.hpp`
- Modify: `plugins/Ouroboros/hmi/Font.hpp` (glyphs `V N G K ? %`), `plugins/Ouroboros/hmi/LoopWidget.hpp` (`loading`), `plugins/Ouroboros/hmi.ttl.in`, `plugins/Ouroboros/OuroborosPlugin.hpp` (slot kind, drawing)
- Test: `tests/test_widget.cpp`, `tests/lv2/test_lv2_hooks.cpp`

**Interfaces:**
- Consumes: `pieceStatus()`, `pieceLoading()`, `fWorker.fullSlots()` (Task 4).
- Produces:
  ```cpp
  struct PiecesView { uint16_t full; int selected; int loaded; const char* status; };   // selected/loaded 0-15, loaded -1: none
  size_t renderPiecesWidget(const PiecesView& v, uint16_t w, uint16_t h, uint8_t* bits);  // Mono1, like renderSongWidget
  ```
  `WidgetView` gains `bool loading` (the loop widget's state text is `LOAD`, over everything, FULL included).
- Layout, 128×37: two rows of 8 cells 16 px wide; numbers 1–8 at rows 0–6, a 5×5 box at rows 8–12 under each number (filled: holds a piece; outline: empty); numbers 9–16 at rows 15–21, boxes at 23–27; the selected slot's number inverted in a 13×9 box; the loaded slot's box underlined at row 13 or 28; the status line at rows 30–36. Below 37 rows (minimum 96×16): only the status line at row 1. Below 16 rows: nothing.
- `hmi.ttl.in`: widget `piece_slots`, name "Pieces", min 96×16, natural 128×37, Mono1. `WidgetSlot::song` becomes `std::atomic<uint8_t> kind` (0 loop, 1 song, 2 pieces). While the engine is lent, the loop widget redraws from the last view with `loading = true`.

- [ ] **Step 1: Write the failing tests** in `test_widget.cpp`: each new glyph renders non-blank and differs from space; the full-size layout (filled vs outline box for slots 1 and 2, inverted selected number, underline under the loaded slot, the status text at row 30); 96×16 shows only the status; 96×15 is blank; guard bytes untouched at 96×16; the loop widget shows `LOAD` with `loading` even when `memoryFull`. lv2 test: `widget_shown "piece_slots"` 128×37 → a whole-area draw arrives within 40 runs.
- [ ] **Step 2: Run, expect failures.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `make test` and `make test-lv2` → both pass. Paste an ASCII dump of the 128×37 widget (slots 1, 2, 5 full; 3 selected; 2 loaded; status `SAVE 3?`) into the report.
- [ ] **Step 5: Commit** `widgets: the pieces widget; LOAD on the loop widget`.

### Task 6: Manual, README and the Dwarf checks

**Files:**
- Modify: `docs/manual/ouroboros-manual.html` (and regenerate `docs/manual/ouroboros-manual.pdf` the way the earlier song-parts commit did), `README.md`

- [ ] **Step 1: Manual:** a "Saved pieces" section after "Song parts": what a piece holds, the slot folders and that pieces can be copied with MOD's file manager, Save (any time; waits for a take; `SAVE N?`), Load (between songs; stops; `LOAD N?`; presses ignored while loading), the remembered piece, tempo (set MOD's tempo to the BPM shown), every status string; the Controls table gains "Page 2: Slot, Save, Load" and the `piece_slots` widget; the Settings table gains Slot, Save, Load. README: one line in the feature list.
- [ ] **Step 2: Dwarf build:** `make dwarf-build` succeeds and `aarch64-modaudio-linux-gnu-readelf -d` on the `.so` (inside `moddwarf-cross`) shows the thread library resolved (or `pthread_create` defined in libc).
- [ ] **Step 3: CPU:** bench `tests/bench/bench.cpp` on x86 and on the Dwarf at the plugin's real flags, pinned, 5 interleaved runs against main, both SYNC modes: within noise; `out=` identical. Copy only to the Dwarf's `/tmp`, no deploy or service restart; delete it afterwards. Write the numbers into the report.
- [ ] **Step 4: Run** `make test` and `make test-lv2` → both pass.
- [ ] **Step 5: Commit** `docs: saved pieces`.
