# Ouroboros Looper Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A 4-track, mono, free-time looper LV2 plugin for the MOD Dwarf, with click-free loop points, delta-based unlimited undo, footswitch labels/LEDs and a Starless display widget. Plus a small mod-ui patch so plugin-moved knobs reach the Dwarf.

**Architecture:**
- A host-independent, header-only engine in `plugins/Ouroboros/dsp/`: `BlockPool` → `Delta` → `Looper`. Unit-tested sample by sample.
- A thin DPF plugin (`OuroborosPlugin.hpp/.cpp`) maps ports and footswitch gestures onto the engine.
- Two HMI pieces in `plugins/Ouroboros/hmi/`: `HmiBridge` for labels/LEDs and `LoopWidget` for the bitmap. They are pure logic with C-struct fakes in tests.
- They reach the host through a small DPF patch (`patches/dpf-lv2-hooks.patch`). The patch passes the LV2 feature array to the plugin constructor and forwards unknown `extension_data` URIs to the plugin.

**Tech Stack:** C++14 (gnu++14), DPF @ `4238e1c7`, LV2, MOD HMI extension (`lv2-hmi.h`), aLabs HMI widgets (`alabs-hmi-widgets.h`). Build and tests via the template Makefile (`make`, `make test` under ASan, `make test-lv2`). Dwarf cross-build via the shared `moddwarf-cross` Docker image. The mod-ui patch is Python 3.4 (device).

**Spec:** `docs/superpowers/specs/2026-11-20-ouroboros-looper-design.md`. Read it first; this plan argues from it.

## Global Constraints

- Mono in, mono out. **Output = loops only.** The dry signal is added only when `dry_thru` is on (default off).
- 4 tracks, all with one loop length L, shared playhead. L is unset while every track is empty. The first recording on any track sets it, and L resets when every track is empty again.
- Max L = **5 minutes**. Reaching it auto-closes the recording. A first recording shorter than **100 ms** is treated as aborted.
- Tail: default **50 ms**, setting range **0–500 ms**, clamped to **≤ L/2**. Only the recording that sets L gets a tail. Punch-outs get a **5 ms** fade and no tail. Record/punch starts get a **3 ms** fade-in. On/off, volume, bypass, dry-thru, undo, redo and clear ramp over **15 ms**. All fades and ramps are linear.
- Volume: **−60…+6 dB**, default 0 dB, **playback only**: it never touches stored audio.
- Undo/redo: every layer undoable, **one layer per press**, applied **lazily at the playhead over one pass** (never O(L) in one block). Silent overdubs whose written peak stays below **5.0e-4 (≈ −66 dBFS)** make no layer.
- Memory budget for undo/redo deltas: default **200 MB**, setting range **16–400 MB**. When full: drop redo (oldest first, all tracks) → merge globally-oldest undo layers → drop the current overdub's delta (it keeps recording).
- Soft clip on the output only, transparent below **0.7079 (−3 dBFS)**.
- Settings: `hold_time` default **500 ms** (200–2000). `trigger_latency` default **3 ms** (0–50). `offset` default **0 ms** (0–30, "write overdubs this much earlier"; see Task 5). `after_rec` Rec→Play (default) / Rec→Dub.
- **No allocation, locking or I/O in `run()`.** The one exception: MOD HMI `set_label`/`set_value`/`set_led_with_blink`, called **only when something changed**. aLabs `draw_bitmap()` is RT-safe.
- Memory is reserved with `std::calloc` (portable; on Linux it gives untouched zero pages, so empty tracks cost no RAM). Never `mmap`: desktop CI builds Windows and macOS.
- **Port symbols and order are frozen from the first release.** Never delete or rename a port; deprecate it hidden (template `INSTRUCTIONS.md`).
- Must run fully on stock MOD OS, MOD Desktop and in VST3/CLAP hosts without the HMI features: no calls, no crash.
- DPF stays pinned at `4238e1c7`. The patch is applied by `make dpf-patch`. `.gitmodules` sets `ignore = dirty` for `dpf` so `make release`'s clean-tree check still passes.
- The device mod-ui is the Starless lineage at `/usr/lib/python3.4/site-packages/mod/host.py`, Python **3.4**: no f-strings.

## Review Focus

1. **The plugin is deactivated/reactivated or the sample rate changes mid-session** (MOD buffer-size change, pedalboard reload). Expected: everything resets to empty with no stale L or pending ops. Test: Task 8 `reactivateResets`.
2. **A saved pedalboard loads** with `dry_thru=1`, non-default volumes or `on_N=0`, arriving with the first `run()`. Expected: applied instantly, with no 15 ms ramp from the defaults. Test: Task 8 `savedValuesSnapOnLoad`.
3. **Mashing footswitches**: undo/redo every block, handovers, clear-all mid-recording. Expected: pending ops stay ≤ `kMaxPendingOps`, states stay consistent, and no NaN/inf or |out| > 1. Tests: Task 5 `undoMashIsBounded`, Task 7 `stressRandomPresses`.
4. **Memory reservation fails** (Patchstorage 32-bit armhf, absurd sample rate). Expected: the plugin stays alive, outputs dry (if `dry_thru`) or silence, and ignores presses. Tests: Task 4 `initFailureIsSafe`, Task 8 `disabledEngineStillPassesDry`.
5. **The host lacks the HMI/widget features** (stock MOD OS, MOD Desktop, VST3/CLAP). Expected: no HMI calls, no crash. Tests: Task 2 `instantiateWithoutFeatures`, Task 9 `noFeatureNoCalls`.

---

## File map

| File | Responsibility |
|---|---|
| `Makefile` | Identity vars, `dpf-patch`, TTL post-processing (momentary + HMI fragment), `test`, `test-lv2` |
| `.gitmodules` | `dpf` submodule, `ignore = dirty` |
| `patches/dpf-lv2-hooks.patch` | The DPF patch (Task 2) |
| `plugins/Ouroboros/Makefile` | DPF inner Makefile, `-std=gnu++14` |
| `plugins/Ouroboros/DistrhoPluginInfo.h` | Identity, mono I/O, `DISTRHO_PLUGIN_WANT_LV2_EXTENSION_DATA 1` |
| `plugins/Ouroboros/OuroborosPlugin.hpp` | Plugin class + `ParamIndex` (tests include it) |
| `plugins/Ouroboros/OuroborosPlugin.cpp` | `createPlugin()`, LV2 feature scan, extension-data interfaces |
| `plugins/Ouroboros/hmi.ttl.in` | TTL fragment appended to the generated TTL |
| `plugins/Ouroboros/dsp/Constants.hpp` | Fixed timing constants + `msToSamples` |
| `plugins/Ouroboros/dsp/BlockPool.hpp` | Fixed-size block allocator over one calloc'd region, budgeted |
| `plugins/Ouroboros/dsp/Delta.hpp` | One overdub layer's added samples, as linked pool blocks |
| `plugins/Ouroboros/dsp/Looper.hpp` | The engine: tracks, L, playhead, state machine, undo, mixing |
| `plugins/Ouroboros/hmi/lv2-hmi.h`, `alabs-hmi-widgets.h` | Vendored extension headers |
| `plugins/Ouroboros/hmi/HmiBridge.hpp` | Footswitch/knob labels and LEDs, sent on change |
| `plugins/Ouroboros/hmi/Font.hpp`, `LoopWidget.hpp` | 5×7 glyphs; Mono1 renderer for the "loop" widget |
| `plugins/Ouroboros/modgui.ttl`, `modgui/*` | Plain functional pedal face |
| `tests/test_util.hpp` | `CHECK`/`REPORT` (copied from Boreas) |
| `tests/signals.hpp` | Signal helpers: sine, slice, peak, steps, diffs |
| `tests/looper_rig.hpp` | Engine test rig (includes `signals.hpp`) |
| `tests/test_*.cpp` | One file per task area |
| `tests/lv2/test_lv2_hooks.cpp` | dlopen's the built `.so` with a fake host (`make test-lv2`) |

---

### Task 1: Repository from the template, renamed, with an Ouroboros skeleton

**Files:**
- Create: `~/Fun/ouroboros/` contents from `~/Fun/mod-plugin-template` (the existing `docs/superpowers/` is kept)
- Modify: `Makefile` (identity vars), `.gitmodules`, `patchstorage.json`, `README.md`
- Rename: `plugins/MyPlugin` → `plugins/Ouroboros`, `modgui/*-myplugin.*` → `*-ouroboros.*`, `docs/manual/myplugin-manual.*` → `ouroboros-manual.*`
- Create: `plugins/Ouroboros/OuroborosPlugin.hpp`, `OuroborosPlugin.cpp` (replacing `MyPluginPlugin.cpp`)
- Delete: `tests/test_gain.cpp`, `tests/test_bypass.cpp`
- Create: `tests/test_util.hpp`, `tests/signals.hpp`, `tests/test_plugin.cpp`

**Interfaces:**
- Produces: `class OuroborosPlugin : public DISTRHO::Plugin` in `OuroborosPlugin.hpp` with `enum ParamIndex { kParamBypass = 0, kNumParams }` (Task 8 replaces the enum). Mono in/out.
- Produces: `tests/test_util.hpp` with `CHECK(cond)`, `REPORT()`, `approx(a,b,eps)`, identical to `~/Fun/boreas/tests/test_util.hpp`.
- Produces (`tests/signals.hpp`, header-only, `inline`):
  ```cpp
  std::vector<float> sine(double hz, float amp, uint32_t n, double sr = 48000.0);
  std::vector<float> slice(const std::vector<float>& x, size_t from, size_t to);
  std::vector<float> wrap(std::vector<float> img);     // appends img[0], so maxStep sees L-1 -> 0
  float maxStep(const std::vector<float>& x); float peak(const std::vector<float>& x);
  float maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b);
  float sineStep(double hz, float amp, double sr = 48000.0);   // 2*pi*hz/sr*amp
  ```

- [ ] **Step 1: Create the repo.**
  ```bash
  cd ~/Fun/ouroboros
  rsync -a --exclude .git --exclude bin --exclude build --exclude dist --exclude dpf ~/Fun/mod-plugin-template/ ./
  git init -b main
  git submodule add https://github.com/DISTRHO/DPF.git dpf && git -C dpf checkout 4238e1c7
  git config -f .gitmodules submodule.dpf.ignore dirty
  ```
  The DSP-only build does not need DPF's nested submodules.

- [ ] **Step 2: Rename.**
  - Top-level `Makefile`: `PLUGIN := ouroboros`, `PLUGIN_DIR := plugins/Ouroboros`, `BRAND := Stefan`, `LABEL := Ouroboros`, `PLUGIN_URI_BASE := http://ouroboros.local/plugins`.
  - `DistrhoPluginInfo.h`:
    - brand `"Stefan"`, name `"Ouroboros"` / `"Ouroboros (Beta)"`;
    - URIs `http://ouroboros.local/plugins/ouroboros[-beta]`;
    - CLAP ids `local.ouroboros.ouroboros[-beta]`;
    - `BRAND_ID Ouro`/`OurB`, `UNIQUE_ID dOur`/`dOuB`;
    - `PLUGIN_HOMEPAGE "https://github.com/stefdoerr/ouroboros"`;
    - category `lv2:UtilityPlugin`, `DISTRHO_PLUGIN_NUM_OUTPUTS 1`;
    - macro `OUROBOROS_BETA`.
  - Inner Makefile: `FILES_DSP = OuroborosPlugin.cpp`, `OUROBOROS_BETA`, and add `CXXFLAGS += -std=gnu++14` (DPF hard-codes gnu++11 before `$(CXXFLAGS)`; the last `-std` wins).
  - Also rename the `modgui.ttl` strings, the modgui file names and the `patchstorage.json` URL.

- [ ] **Step 3: Write the failing test** `tests/test_plugin.cpp`. Use the template harness (`d_nextBufferSize=128`, `d_nextSampleRate=48000`, `d_nextCanRequestParameterValueChanges=true`, `PluginExporter plugin(nullptr, nullptr, requestStub, nullptr)`), include `signals.hpp`, and write a local `run(plugin, vec)` that processes `vec` in 128-frame blocks into `gOut`.
  ```cpp
  static void skeletonIsMonoAndSilent() {
      CHECK(DISTRHO_PLUGIN_NUM_INPUTS == 1 && DISTRHO_PLUGIN_NUM_OUTPUTS == 1);
      // 0.1 s of sine in, nothing recorded -> silence out (loops only)
      plugin.activate(); run(plugin, sine(220, 0.1f, 4800));
      CHECK(peak(gOut) == 0.0f);
  }
  int main() { skeletonIsMonoAndSilent(); REPORT(); }
  ```

- [ ] **Step 4: Run it.** `make test` → FAIL (`OuroborosPlugin.hpp` missing or output not silent).

- [ ] **Step 5: Write the skeleton.** `OuroborosPlugin` keeps the template's info methods, version macros, `getUniqueId()` (`d_cconst('d','O','u','r')`, beta `('d','O','u','B')`), the bypass designation parameter, and a `run()` that writes zeros. The class goes in the header and `createPlugin()` in the .cpp.

- [ ] **Step 6: Verify.**
  - `make test` → `OK`, `all tests passed`.
  - `make` → `bin/ouroboros.lv2/ouroboros.ttl` contains `http://ouroboros.local/plugins/ouroboros`.
  - `grep -ri myplugin . --exclude-dir=dpf --exclude-dir=.git` → no output.

- [ ] **Step 7: Commit.** `git add -A && git commit -m "Ouroboros: start from mod-plugin-template (skeleton, spec, plan)"`

---

### Task 2: DPF patch, LV2 glue and HMI TTL fragment

**Files:**
- Create: `patches/dpf-lv2-hooks.patch`, `plugins/Ouroboros/hmi/lv2-hmi.h`, `plugins/Ouroboros/hmi/alabs-hmi-widgets.h`, `plugins/Ouroboros/hmi.ttl.in`, `tests/lv2/test_lv2_hooks.cpp`
- Modify: `Makefile` (`dpf-patch`, `ttl`, `test-lv2`), `DistrhoPluginInfo.h`, `OuroborosPlugin.hpp/.cpp`, `DEVELOPERS.md` (one paragraph on the patch)

**Interfaces:**
- Produces (DPF, after the patch):
  - `namespace DISTRHO { extern const void* d_nextLv2Features; }` in `DistrhoPlugin.hpp`. It is the `const LV2_Feature* const*` of the instance being constructed, non-null only inside the `Plugin` constructor of an LV2 build.
  - When `DISTRHO_PLUGIN_WANT_LV2_EXTENSION_DATA` is 1, the plugin must define `const void* DISTRHO::d_lv2PluginExtensionData(const char* uri, void* (*instanceOf)(void* lv2Handle));`. `instanceOf` maps an `LV2_Handle` to the `Plugin*`.
- Produces (plugin): `OuroborosPlugin` members `const LV2_HMI_WidgetControl* fHmiWc = nullptr;` and `const ALABS_HMI_WidgetDraw* fWidgetDraw = nullptr;` (set from `d_nextLv2Features` in the constructor). Also these methods called from the extension callbacks: `void hmiAddressed(uint32_t lv2Index, LV2_HMI_Addressing a, const LV2_HMI_AddressingInfo* info)`, `void hmiUnaddressed(uint32_t lv2Index)`, `void widgetShown(const char* symbol, ALABS_HMI_Widget w, const ALABS_HMI_WidgetArea* area)`, `void widgetHidden(ALABS_HMI_Widget w)`. Task 9 fills the HMI ones and Task 10 the widget ones.
- Produces: `uint32_t paramFromLv2Index(uint32_t lv2Index)` = `lv2Index - (DISTRHO_PLUGIN_NUM_INPUTS + DISTRHO_PLUGIN_NUM_OUTPUTS)`. Valid because the plugin has no event ports (no state, MIDI or time position).

- [ ] **Step 1: Vendor the headers.**
  ```bash
  cp ~/Fun/mod/mod-host/src/lv2/lv2-hmi.h plugins/Ouroboros/hmi/
  gh api 'repos/sejerpz/alabs-hmi-widgets/contents/alabs-hmi-widgets.lv2/alabs-hmi-widgets.h' -H 'Accept: application/vnd.github.raw' > plugins/Ouroboros/hmi/alabs-hmi-widgets.h
  ```

- [ ] **Step 2: Write the failing test** `tests/lv2/test_lv2_hooks.cpp`, with a `Makefile` target:
  ```make
  test-lv2: all
  	@mkdir -p build/tests
  	g++ -std=gnu++14 -g -O0 -fsanitize=address -Idpf/distrho/src -I$(PLUGIN_DIR)/hmi tests/lv2/test_lv2_hooks.cpp -ldl -o build/tests/test_lv2_hooks
  	build/tests/test_lv2_hooks $(BUNDLE)/$(BUNDLE_NAME).so
  .PHONY: test-lv2
  ```
  The fake host provides:
  - `LV2_OPTIONS__options` with `LV2_BUF_SIZE__nominalBlockLength = 128`;
  - a string-table `LV2_URID__map`;
  - optionally a recording fake `LV2_HMI_WidgetControl` and a recording fake `ALABS_HMI_WidgetDraw` (it stores every `draw_bitmap` rect and whether `bits` was null).

  It connects ports 0 (in) and 1 (out) to 128-float buffers, and every other index up to 64 to a float array holding each port's default.
  ```cpp
  static void extensionDataForwarded(const LV2_Descriptor* d) {
      CHECK(d->extension_data(LV2_HMI__PluginNotification) != nullptr);
      CHECK(d->extension_data(ALABS_HMI__WidgetNotification) != nullptr);
      CHECK(d->extension_data("urn:ouroboros:nothing") == nullptr);
  }
  static void instantiateWithoutFeatures(const LV2_Descriptor* d) {     // Review Focus 5
      LV2_Handle h = d->instantiate(d, 48000, bundleDir, baseFeatures);  // options + urid only
      CHECK(h != nullptr); connectAll(d, h); d->activate(h);
      for (int i = 0; i < 100; ++i) d->run(h, 128);                     // no crash, no HMI calls possible
      d->cleanup(h);
  }
  static void widgetShownLeadsToDraw(const LV2_Descriptor* d) {
      LV2_Handle h = d->instantiate(d, 48000, bundleDir, fullFeatures);
      connectAll(d, h); d->activate(h);
      auto wn = (const ALABS_HMI_WidgetNotification*)d->extension_data(ALABS_HMI__WidgetNotification);
      ALABS_HMI_WidgetArea area = { sizeof(area), 40, 37, ALABS_HMI_PixelFormat_Mono1 };
      wn->widget_shown(h, "loop", (ALABS_HMI_Widget)0x1, &area);
      for (int i = 0; i < 20; ++i) d->run(h, 128);
      CHECK(!gDraws.empty());
      for (auto& r : gDraws) CHECK(r.x + r.w <= 40 && r.y + r.h <= 37 && r.widget == (ALABS_HMI_Widget)0x1);
      wn->widget_hidden(h, (ALABS_HMI_Widget)0x1); size_t n = gDraws.size();
      for (int i = 0; i < 20; ++i) d->run(h, 128);
      CHECK(gDraws.size() == n);
      d->cleanup(h);
  }
  ```

- [ ] **Step 3: Run it.** `make test-lv2` → FAIL (`extension_data` returns null).

- [ ] **Step 4: Patch DPF**, then save the diff with `git -C dpf diff > patches/dpf-lv2-hooks.patch`:
  - `distrho/src/DistrhoPluginChecks.h`: default `DISTRHO_PLUGIN_WANT_LV2_EXTENSION_DATA 0`.
  - `distrho/DistrhoPlugin.hpp`: declare both symbols from Interfaces, the second under the macro.
  - `distrho/src/DistrhoPlugin.cpp`: `const void* d_nextLv2Features = nullptr;`.
  - `distrho/src/DistrhoPluginLV2.cpp`:
    - in `lv2_instantiate`, set `d_nextLv2Features = features;` before `new PluginLv2(...)`, and reset it to `nullptr` right after;
    - add a public `void* getPluginInstancePointer() const noexcept { return fPlugin.getInstancePointer(); }` to `PluginLv2`;
    - add a static `lv2_instance_of(void* h)` calling it;
    - in `lv2_extension_data`, just before the final `return`, add `#if DISTRHO_PLUGIN_WANT_LV2_EXTENSION_DATA if (const void* e = d_lv2PluginExtensionData(uri, lv2_instance_of)) return e; #endif`.

  Makefile:
  ```make
  dpf-patch:
  	@grep -q d_nextLv2Features dpf/distrho/src/DistrhoPlugin.cpp || git -C dpf apply ../patches/dpf-lv2-hooks.patch
  ```
  Make `plugin`, `features`, `test`, `dwarf-build` and `patchstorage-build` depend on `dpf-patch`. The grep makes it a no-op inside the read-only build containers.

- [ ] **Step 5: Write the plugin glue.**
  - Set `DISTRHO_PLUGIN_WANT_LV2_EXTENSION_DATA 1`.
  - Constructor: scan `d_nextLv2Features` (cast to `const LV2_Feature* const*`) for `LV2_HMI__WidgetControl` and `ALABS_HMI__WidgetDraw`.
  - `d_lv2PluginExtensionData`:
    - store `instanceOf` in a file-static;
    - return static `LV2_HMI_PluginNotification` and `ALABS_HMI_WidgetNotification` structs whose callbacks cast `instanceOf(handle)` to `OuroborosPlugin*` and call the four methods above;
    - return `nullptr` for any other URI.
  - Until Task 10, `widgetShown` records `{widget, w, h}` in slot 0 (publish the handle last via `std::atomic` release). `run()` clears a shown slot once with `draw_bitmap(w, 0, 0, w, h, nullptr)`.
  - VST3/CLAP builds never call `d_lv2PluginExtensionData` and see `d_nextLv2Features == nullptr`.

- [ ] **Step 6: Write the TTL fragment** `plugins/Ouroboros/hmi.ttl.in`. Append it in the `ttl` target after `generate-ttl.sh`:
  `sed 's|@PLUGIN_URI@|$(PLUGIN_URI)|' $(PLUGIN_DIR)/hmi.ttl.in >> $(BUNDLE)/$(BUNDLE_NAME).ttl`.
  ```turtle
  @prefix ahw: <https://github.com/sejerpz/alabs-hmi-widgets#> .
  @prefix hmi: <http://moddevices.com/ns/hmi#> .
  @prefix lv2: <http://lv2plug.in/ns/lv2core#> .
  <@PLUGIN_URI@>
      lv2:optionalFeature hmi:WidgetControl , ahw:WidgetDraw ;
      lv2:extensionData hmi:PluginNotification , ahw:WidgetNotification ;
      ahw:widget [ a ahw:Widget ; lv2:symbol "loop" ; lv2:name "Loop" ;
                   ahw:minWidth 40 ; ahw:minHeight 8 ; ahw:naturalWidth 40 ; ahw:naturalHeight 37 ;
                   ahw:pixelFormat ahw:Mono1 ] .
  ```
  - mod-host only queries `PluginNotification` when `lv2:extensionData` declares it (`effects.c:5520`), and Starless 19 does the same for `WidgetNotification`.
  - No `ahw:suggestedPorts`: auto-binding would put `onoff` on a footswitch of another page.

- [ ] **Step 7: Verify.**
  - `make test-lv2` → `OK`.
  - `make` builds `bin/ouroboros.vst3` and `bin/ouroboros.clap` (the patch compiles for every format).
  - `make test` → passes.
  - `grep -c 'ahw:widget' bin/ouroboros.lv2/ouroboros.ttl` → `1`.
  - `git status --short` does not list `dpf`.

- [ ] **Step 8: Commit.** `git add -A && git commit -m "DPF patch: pass LV2 features and extension data to the plugin; HMI TTL fragment"`

---

### Task 3: BlockPool and Delta

**Files:**
- Create: `plugins/Ouroboros/dsp/BlockPool.hpp`, `plugins/Ouroboros/dsp/Delta.hpp`, `tests/test_pool.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace ouro {
  class BlockPool {                              // single-threaded: audio thread only after reserve()
  public:
      static constexpr uint32_t kBlock = 4096;   // samples per block (16 KiB)
      bool     reserve(size_t maxBytes);         // non-RT; std::calloc data + next-links; false on failure
      void     setBudgetBytes(size_t bytes);     // blocks allowed = min(bytes / (kBlock*4), reserved blocks)
      int32_t  alloc();                          // RT; returns a ZEROED block, or -1 at budget
      void     free(int32_t b);                  // RT
      float*   data(int32_t b);  const float* data(int32_t b) const;
      int32_t& next(int32_t b);                  // link used by Delta
      uint32_t usedBlocks() const;  uint32_t budgetBlocks() const;
  };
  struct DeltaCursor { int32_t block = -1; uint32_t index = 0; };   // index = block number within the delta
  class Delta {                                  // samples one overdub added, at loop positions start..start+length-1 (mod L)
  public:
      void     begin(uint32_t startPos);                                      // empty
      bool     append(BlockPool& p, float v);                                 // offset = length(); false = pool full (sample not stored)
      void     accumulate(BlockPool& p, DeltaCursor& c, uint32_t offset, float v);   // offset < length()
      float    read(const BlockPool& p, DeltaCursor& c, uint32_t offset) const;      // offset < length()
      uint32_t start() const;  uint32_t length() const;  uint32_t blocks() const;
      void     release(BlockPool& p);                                         // frees every block, length 0
  };
  }
  ```
- Blocks are layer-relative: block k holds offsets `[k*kBlock, (k+1)*kBlock)`. Cursors walk the links forward and restart from the head when the offset goes backwards, so sequential access is O(1).

- [ ] **Step 1: Write the failing tests** `tests/test_pool.cpp`:
  ```cpp
  static void budgetLimitsAllocs() {
      ouro::BlockPool p; CHECK(p.reserve(1 << 20));
      p.setBudgetBytes(16 * ouro::BlockPool::kBlock * 4);
      for (int i = 0; i < 16; ++i) CHECK(p.alloc() >= 0);
      CHECK(p.alloc() == -1);  p.free(3);  CHECK(p.alloc() >= 0);  CHECK(p.usedBlocks() == 16);
  }
  static void reusedBlocksAreZeroed() {
      ouro::BlockPool p; p.reserve(1 << 20); p.setBudgetBytes(1 << 20);
      int32_t b = p.alloc(); p.data(b)[100] = 1.0f; p.free(b);
      int32_t c = p.alloc(); CHECK(c == b && p.data(c)[100] == 0.0f);
  }
  static void deltaAppendReadAccumulate() {
      ouro::BlockPool p; p.reserve(1 << 20); p.setBudgetBytes(1 << 20);
      ouro::Delta d; d.begin(777);
      for (uint32_t i = 0; i < 10000; ++i) CHECK(d.append(p, float(i)));
      CHECK(d.length() == 10000 && d.blocks() == 3 && d.start() == 777);
      ouro::DeltaCursor c;
      CHECK(d.read(p, c, 9999) == 9999.0f && d.read(p, c, 0) == 0.0f && d.read(p, c, 5000) == 5000.0f);
      d.accumulate(p, c, 5000, 0.5f);  CHECK(d.read(p, c, 5000) == 5000.5f);
      d.release(p);  CHECK(p.usedBlocks() == 0 && d.length() == 0);
  }
  static void appendStopsWhenPoolFull() {
      ouro::BlockPool p; p.reserve(1 << 20); p.setBudgetBytes(2 * ouro::BlockPool::kBlock * 4);
      ouro::Delta d; d.begin(0); uint32_t n = 0;
      while (d.append(p, 1.0f)) ++n;
      CHECK(n == 2 * ouro::BlockPool::kBlock && d.length() == n);
  }
  static void reserveFailureIsSafe() {
      ouro::BlockPool p; CHECK(!p.reserve(SIZE_MAX / 2)); CHECK(p.alloc() == -1);
  }
  ```

- [ ] **Step 2: Run them.** `make test` → FAIL (headers missing).

- [ ] **Step 3: Implement** both headers per the Interfaces. `alloc()` takes from a free-list stack first, then the never-used high-water mark. Zero the block with `std::memset` (16 KiB per allocation, at most once per ~85 ms of overdub). Free-list and next-link arrays are sized to the reserved block count inside `reserve()`.

- [ ] **Step 4: Run them.** `make test` → `test_pool` prints `OK`.

- [ ] **Step 5: Commit.** `git add -A && git commit -m "engine: BlockPool and Delta"`

---

### Task 4: Looper — first recording, loop length, playback, seam

**Files:**
- Create: `plugins/Ouroboros/dsp/Constants.hpp`, `plugins/Ouroboros/dsp/Looper.hpp`, `tests/looper_rig.hpp`, `tests/test_record.cpp`

**Interfaces:**
- Consumes: `BlockPool`, `Delta` (Task 3). Task 4 only reserves the pool; Task 5 uses it.
- Produces:
  ```cpp
  // Constants.hpp
  namespace ouro {
  constexpr int      kTracks = 4;
  constexpr float    kFadeInMs = 3.0f, kPunchOutFadeMs = 5.0f, kRampMs = 15.0f;
  constexpr float    kMinLoopMs = 100.0f, kMaxLoopSeconds = 300.0f, kHistoryMs = 100.0f;
  constexpr float    kSilenceFloor = 5.0e-4f, kSoftClipKnee = 0.7079f, kMaxBudgetMb = 400.0f;
  constexpr uint32_t kMaxPendingOps = 32;
  inline uint32_t msToSamples(float ms, double sr) { return uint32_t(ms * sr / 1000.0 + 0.5); }
  }
  // Looper.hpp
  namespace ouro {
  enum class TrackState : uint8_t { Empty, Recording, Playing, Overdubbing };
  enum class NextAction : uint8_t { Rec, Play, Dub };
  class Looper {
  public:
      bool init(double sampleRate, uint32_t maxBlock);  // non-RT; false => disabled (process outputs dry/silence, controls ignored)
      void reset();                                     // O(1): all Empty, L unset, no pending ops
      void process(const float* in, float* out, uint32_t n);
      void pressRec(int t);
      void undo(int t);
      void setTailMs(float ms); void setTriggerLatencyMs(float ms); void setRecToDub(bool on);
      TrackState state(int t) const;  NextAction nextAction(int t) const;
      uint32_t loopLength() const;  uint32_t position() const;  uint32_t recordedSamples() const;
      double sampleRate() const;  bool enabled() const;
  };
  }
  ```
  `init()` reserves per track `kMaxLoopSeconds*sr + sr` floats (5 min plus 1 s slack for the tail and history), a `kHistoryMs` input ring, and the pool at `kMaxBudgetMb` (retry at half the size down to 16 MB on failure). It sets the default budget to 200 MB. Controls take effect at the start of the next `process()` call.
- Produces (`tests/looper_rig.hpp`):
  ```cpp
  struct Rig {                                         // trigger latency forced to 0 unless a test sets it
      ouro::Looper lp; double sr; uint32_t block; std::vector<float> out;
      explicit Rig(double sr = 48000.0, uint32_t block = 128);
      void feed(const std::vector<float>& in);         // block-sized process() calls; appends to out
      void silence(uint32_t n);
      std::vector<float> capture(uint32_t n);          // feed n zeros, return those outputs
      std::vector<float> loopImage();                  // one pass of silence, indexed by loop position
      std::vector<float> settle();                     // one pass of silence, then loopImage()
  };
  float fadeInGain(uint32_t i, double sr = 48000.0);   // linear 0->1 over kFadeInMs
  std::vector<float> recordBase(Rig& r, uint32_t n, float amp);   // track 0: press, sine(220, amp, n), press; returns loopImage()
  ```
  `looper_rig.hpp` includes `signals.hpp` and `test_util.hpp`.

**Algorithm (the tests don't pin all of it):**
- **Read before write.** At each sample, a track's output is its buffer value *before* the input is added. The player hears themselves through their dry path, never doubled.
- **First recording (L unset).** On press, copy the last D = `trigger_latency` input samples from the history ring to `buf[0..D)` and continue linearly. The buffer is *overwritten* (`buf[r] = x*env`), because reused pages hold old audio. Apply a fade-in over `kFadeInMs`.
- **On the closing press:** set `L = r - D`. If L < `kMinLoopMs`, abort. The playhead becomes D.
  - The D samples already recorded past the close are tail: fold them now (`buf[i] += buf[L+i]*g(i)`, i < D).
  - For p in [D, T) the tail goes straight in: `out = buf[p]; buf[p] += x*g(p)`, with g linear 1→0 over T = min(tail, L/2).
  - The state becomes Playing (or Overdubbing in Rec→Dub, Task 5).
- **Auto-close.** When `r` reaches `kMaxLoopSeconds*sr`, close as if B were pressed.
- **Undo while recording.** Abort: the track goes Empty and L stays unset.

- [ ] **Step 1: Write the failing tests** `tests/test_record.cpp`:
  ```cpp
  static void recordSetsLengthAndPlays() {
      Rig r; auto in = sine(220, 0.1f, 24064);
      r.lp.pressRec(0); r.feed(in); r.lp.pressRec(0);
      CHECK(r.lp.loopLength() == 24064);
      CHECK(r.lp.state(0) == ouro::TrackState::Playing && r.lp.nextAction(0) == ouro::NextAction::Dub);
  }
  static void loopPlaysRecordedAudio() {
      Rig r; r.lp.setTailMs(0); auto in = sine(220, 0.1f, 24064);
      r.lp.pressRec(0); r.feed(in); r.lp.pressRec(0);
      auto img = r.loopImage();
      for (uint32_t i = 0; i < 24064; ++i) CHECK(std::fabs(img[i] - in[i] * fadeInGain(i)) < 1e-6f);
  }
  static void backDatingKeepsLengthAndShiftsStart() {
      Rig r; r.lp.setTailMs(0); r.lp.setTriggerLatencyMs(2.0f);       // D = 96
      auto in = sine(220, 0.1f, 48000);
      r.feed(slice(in, 0, 4096)); r.lp.pressRec(0); r.feed(slice(in, 4096, 28160)); r.lp.pressRec(0);
      CHECK(r.lp.loopLength() == 24064);
      auto img = r.loopImage();
      CHECK(maxAbsDiff(slice(img, 200, 20000), slice(in, 4000 + 200, 4000 + 20000)) < 1e-6f);
  }
  static void seamIsContinuous() {
      Rig r; auto in = sine(220, 0.1f, 48000);                       // tail 50 ms (default)
      r.lp.pressRec(0); r.feed(slice(in, 0, 24064)); r.lp.pressRec(0);
      r.feed(slice(in, 24064, 24064 + 4864));                        // keep playing through the tail
      CHECK(maxStep(wrap(r.loopImage())) < 2.5f * sineStep(220, 0.1f));
  }
  static void tailClampedOnShortLoop() {
      Rig r; r.lp.setTailMs(500); auto in = sine(220, 0.1f, 48000);
      r.lp.pressRec(0); r.feed(slice(in, 0, 7168)); r.lp.pressRec(0); r.feed(slice(in, 7168, 31168));
      auto img = r.loopImage();
      CHECK(r.lp.loopLength() == 7168);
      CHECK(maxStep(wrap(img)) < 2.5f * sineStep(220, 0.1f) && peak(img) < 0.25f);
  }
  static void tooShortIsAborted() {
      Rig r; r.lp.pressRec(0); r.feed(sine(220, 0.1f, 4736)); r.lp.pressRec(0);   // 98.7 ms
      CHECK(r.lp.loopLength() == 0 && r.lp.state(0) == ouro::TrackState::Empty);
  }
  static void undoWhileRecordingAborts() {
      Rig r; r.lp.pressRec(0); r.feed(sine(220, 0.1f, 24064)); r.lp.undo(0);
      CHECK(r.lp.state(0) == ouro::TrackState::Empty && r.lp.loopLength() == 0);
      CHECK(peak(r.capture(4800)) == 0.0f);
  }
  static void elapsedCountsWhileRecording() {
      Rig r; r.lp.pressRec(0); r.silence(4864); CHECK(r.lp.recordedSamples() == 4864);
  }
  static void maxLengthAutoCloses() {
      Rig r(8000.0); r.lp.pressRec(0); r.silence(8000 * 301);
      CHECK(r.lp.state(0) == ouro::TrackState::Playing && r.lp.loopLength() == 8000u * 300u);
  }
  static void initFailureIsSafe() {                                   // Review Focus 4
      ouro::Looper lp; CHECK(!lp.init(1.0e12, 128)); CHECK(!lp.enabled());
      float in[128], out[128]; std::fill(in, in + 128, 0.3f);
      lp.pressRec(0); lp.process(in, out, 128);
      CHECK(out[0] == 0.0f && lp.state(0) == ouro::TrackState::Empty);
  }
  ```

- [ ] **Step 2: Run them.** `make test` → FAIL (headers missing).

- [ ] **Step 3: Implement** `Constants.hpp`, `Looper.hpp` (the first-recording path only) and `looper_rig.hpp`, per the Interfaces and Algorithm. `process()` on a disabled engine writes zeros (Task 7 adds dry-thru).

- [ ] **Step 4: Run them.** `make test` → `test_record` `OK`.

- [ ] **Step 5: Commit.** `git add -A && git commit -m "engine: first recording, loop length, tail-blended seam"`

---

### Task 5: Overdub layers, lazy undo/redo, silent overdubs, Rec→Dub

**Files:**
- Modify: `plugins/Ouroboros/dsp/Looper.hpp`
- Create: `tests/test_overdub.cpp`

**Interfaces:**
- Consumes: Task 4 `Looper`, `Rig`; Task 3 `BlockPool`/`Delta`.
- Produces (added to `Looper`): `void redo(int t)`, `bool canUndo(int t) const`, `bool canRedo(int t) const`, `uint32_t undoDepth(int t) const` (overdub layers on the undo stack, base excluded), `uint32_t pendingOps(int t) const`, `void setAdvanceMs(float ms)`. `pressRec` now handles Playing→Overdubbing→Playing on a track with content.

**Algorithm:**
- **Layers.** Each track has a base (in its buffer, no delta), an undo stack of overdub `Delta`s and a redo stack. A layer = one DUB…PLAY press pair, however many passes it wraps (first pass `append`, later passes `accumulate`).
- **Overdub writes.** At playhead p: `out = buf[p]; v = x*env; buf[p] += v; delta += v`.
  - The env is a 3 ms fade-in at punch-in and a 5 ms fade-out after the PLAY press; the punch-out is not back-dated.
  - Punch-in is back-dated by D: add the last D history samples at positions p−D…p−1 at the press.
  - **Offset (`advanceMs`).** Writes go to `p − advance` (behind the playhead only, so a write never lands where it will be read again this pass). The spec said "signed"; writing ahead would double the input on the current pass, so it is earlier-only (0–30 ms).
- **Pending ops.** Undo/redo create a pending op {delta, sign, its own `DeltaCursor`, start p}. At each sample, *before reading*, every pending op whose span covers p commits `buf[p] ±= delta(p)`. An op is done after one full pass.
  - For the first `kRampMs` after the op starts, output `buf[p] ∓ delta(p)*(1−ramp)`, so the audible change ramps.
  - Ops commute, so several can run at once. If `kMaxPendingOps` are active, further undo/redo presses are ignored.
  - A delta's blocks are released only when no pending op references it and it is in neither stack.
- **Undo** while Overdubbing cancels the overdub: subtract the current delta (pending op with ramp) and discard it (no redo). Undo with an empty undo stack undoes the **base**: the track goes Empty (output ramps out) and the buffer is kept for redo. Task 7 handles L reset and buffer reuse.
- **Redo** pops the redo stack (base → the track is non-empty again).
- **A new overdub on a track clears that track's redo stack.**
- **Silent overdubs.** Track the peak of `|v|` over the layer. At punch-out, if it is below `kSilenceFloor`, subtract the delta (pending op, no ramp needed) and push nothing.
- **Rec→Dub.** On close, the track goes to Overdubbing with a new layer. For the T samples of the tail, the input splits: `g·x` into the base (the tail fold) and `(1−g)·x` into the new layer. The punch-out of an overdub that continues into a new layer uses the same split over the 5 ms fade.

- [ ] **Step 1: Write the failing tests** `tests/test_overdub.cpp`. `recordBase` comes from `looper_rig.hpp`; every test sets tail 0 first.
  ```cpp
  static void undoRestoresExactly() {
      Rig r; r.lp.setTailMs(0); auto base = recordBase(r, 24064, 0.1f);
      r.lp.pressRec(0); r.feed(sine(330, 0.1f, 9600)); r.lp.pressRec(0);
      auto dubbed = r.settle();  CHECK(maxAbsDiff(dubbed, base) > 0.05f && r.lp.undoDepth(0) == 1);
      r.lp.undo(0);  CHECK(maxAbsDiff(r.settle(), base) < 1e-5f);
      r.lp.redo(0);  CHECK(maxAbsDiff(r.settle(), dubbed) < 1e-5f);
  }
  static void undoIsLazy() {
      Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.1f);
      r.lp.pressRec(0); r.feed(sine(330, 0.1f, 24064)); r.lp.pressRec(0); r.settle();
      r.lp.undo(0); r.silence(128);  CHECK(r.lp.pendingOps(0) == 1);
      r.silence(24064);              CHECK(r.lp.pendingOps(0) == 0);
  }
  static void undoIsClickFree() {
      Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.1f);
      r.lp.pressRec(0); r.feed(sine(330, 0.3f, 30000)); r.lp.pressRec(0); r.settle();
      r.lp.undo(0);
      CHECK(maxStep(r.capture(9600)) < 2.5f * (sineStep(330, 0.3f) + sineStep(220, 0.1f)));
  }
  static void undoWhileOverdubbingCancels() {
      Rig r; r.lp.setTailMs(0); auto base = recordBase(r, 24064, 0.1f);
      r.lp.pressRec(0); r.feed(sine(330, 0.1f, 4800)); r.lp.undo(0);
      CHECK(r.lp.state(0) == ouro::TrackState::Playing && !r.lp.canRedo(0));
      CHECK(maxAbsDiff(r.settle(), base) < 1e-5f);
  }
  static void onePressOneLayer() {
      Rig r; r.lp.setTailMs(0); auto base = recordBase(r, 24064, 0.1f);
      r.lp.pressRec(0); r.feed(sine(330, 0.1f, 60160)); r.lp.pressRec(0);   // 2.5 passes
      CHECK(r.lp.undoDepth(0) == 1); r.lp.undo(0); CHECK(maxAbsDiff(r.settle(), base) < 1e-5f);
  }
  static void silentOverdubMakesNoLayer() {
      Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.1f);
      r.lp.pressRec(0); r.feed(std::vector<float>(9600, 1e-5f)); r.lp.pressRec(0); r.settle();
      CHECK(r.lp.undoDepth(0) == 0);
      r.lp.pressRec(0); r.feed(sine(330, 0.01f, 9600)); r.lp.pressRec(0);
      CHECK(r.lp.undoDepth(0) == 1);
  }
  static void punchesAreClickFree() {
      Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.0f);       // silent loop
      auto s = sine(330, 0.3f, 9637); r.lp.pressRec(0); r.feed(slice(s, 37, 9637)); r.lp.pressRec(0);
      CHECK(maxStep(wrap(r.settle())) < 2.5f * sineStep(330, 0.3f));
  }
  static void recToDubUndoIsSeamless() {
      Rig r; r.lp.setRecToDub(true); auto in = sine(220, 0.1f, 48000);
      r.lp.pressRec(0); r.feed(slice(in, 0, 24064)); r.lp.pressRec(0);
      CHECK(r.lp.state(0) == ouro::TrackState::Overdubbing);
      r.feed(slice(in, 24064, 33664)); r.lp.pressRec(0); r.lp.undo(0);
      CHECK(maxStep(wrap(r.settle())) < 2.5f * sineStep(220, 0.1f));
  }
  static void advanceWritesEarlier() {
      // same 2 ms burst overdubbed twice on a silent loop, with offset 0 and 1 ms; the onset moves 48 samples earlier
      CHECK(std::abs(onsetWith(0.0f) - onsetWith(1.0f) - 48) <= 1);
  }
  static void newOverdubDiscardsRedo() {
      Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.1f);
      r.lp.pressRec(0); r.feed(sine(330, 0.1f, 4800)); r.lp.pressRec(0); r.lp.undo(0); r.settle();
      r.lp.pressRec(0); r.feed(sine(440, 0.1f, 4800)); r.lp.pressRec(0);
      CHECK(!r.lp.canRedo(0));
  }
  static void undoMashIsBounded() {                                   // Review Focus 3
      Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.1f);
      r.lp.pressRec(0); r.feed(sine(330, 0.1f, 9600)); r.lp.pressRec(0); auto dubbed = r.settle();
      for (int i = 0; i < 40; ++i) { if (i % 2) r.lp.redo(0); else r.lp.undo(0); r.silence(128);
                                     CHECK(r.lp.pendingOps(0) <= ouro::kMaxPendingOps); }
      CHECK(maxAbsDiff(r.settle(), dubbed) < 1e-5f);                 // even count ends on redo
  }
  ```

- [ ] **Step 2: Run them.** `make test` → FAIL (`redo`/`undoDepth` undefined).

- [ ] **Step 3: Implement** per the Interfaces and Algorithm.

- [ ] **Step 4: Run them.** `make test` → `test_overdub` and `test_record` `OK`.

- [ ] **Step 5: Commit.** `git add -A && git commit -m "engine: overdub layers, lazy undo/redo, silent overdubs, Rec->Dub"`

---

### Task 6: Memory budget and eviction

**Files:**
- Modify: `plugins/Ouroboros/dsp/Looper.hpp`
- Create: `tests/test_budget.cpp`

**Interfaces:**
- Consumes: Task 5 layers; Task 3 `BlockPool::setBudgetBytes`.
- Produces (added to `Looper`): `void setBudgetMb(float mb)` (float so tests can use fractions), `size_t undoBytes() const` (blocks in use × 16 KiB).

**Algorithm:** when `Delta::append` needs a block and `alloc()` returns −1, free space in this order and retry:
1. Release the oldest redo entry across all tracks (layers carry a global sequence number).
2. Merge the globally-oldest undo layer: drop its delta, keep the buffer as is. Skip layers referenced by a pending op.
3. If nothing is left to free, the current overdub stops storing its delta and releases it. It keeps writing to the buffer and is not pushed as a layer.

Lowering the budget below current use only takes effect at the next allocation.

- [ ] **Step 1: Write the failing tests** `tests/test_budget.cpp`. Use budget 0.25 MB = 16 blocks and L = 48000 (a full layer = 12 blocks). The helper `dub(Rig&, t, n)` overdubs `sine(330, 0.1f, n)` on track t; the 5 ms punch-out adds 240 samples.
  ```cpp
  static void fullLayersEvictOldest() {
      Rig r; r.lp.setTailMs(0); r.lp.setBudgetMb(0.25f); recordBase(r, 48000, 0.1f);
      dub(r, 0, 48000); CHECK(r.lp.undoDepth(0) == 1);
      dub(r, 0, 48000); CHECK(r.lp.undoDepth(0) == 1);
      CHECK(r.lp.undoBytes() <= 262144);
  }
  static void redoDroppedBeforeUndo() {
      Rig r; r.lp.setTailMs(0); r.lp.setBudgetMb(0.25f); recordBase(r, 48000, 0.1f);
      dub(r, 0, 14400); dub(r, 0, 14400); r.lp.undo(0); r.settle();   // A, B(redo): 4 + 4 blocks
      dub(r, 0, 36000);                                              // C: 9 blocks
      CHECK(r.lp.undoDepth(0) == 2 && !r.lp.canRedo(0));             // A kept, B dropped
  }
  static void oversizedOverdubKeepsRecording() {
      Rig r; r.lp.setTailMs(0); r.lp.setBudgetMb(0.05f); auto base = recordBase(r, 48000, 0.1f);
      dub(r, 0, 48000);
      CHECK(r.lp.undoDepth(0) == 0 && maxAbsDiff(r.settle(), base) > 0.05f);
  }
  ```
  The cross-track case (`globallyOldestMerged`) needs a second track, so Task 7 adds it.

- [ ] **Step 2: Run them.** `make test` → FAIL.

- [ ] **Step 3: Implement** per the Algorithm.

- [ ] **Step 4: Run them.** `make test` → `test_budget` `OK` (three tests).

- [ ] **Step 5: Commit.** `git add -A && git commit -m "engine: undo memory budget with redo-first, globally-oldest eviction"`

---

### Task 7: Multiple tracks, handover, on/off, volume, mixing, clear

**Files:**
- Modify: `plugins/Ouroboros/dsp/Looper.hpp`, `tests/looper_rig.hpp` (add `recordBaseOn`), `tests/test_budget.cpp` (add `globallyOldestMerged`)
- Create: `tests/test_tracks.cpp`

**Interfaces:**
- Consumes: Tasks 4–6.
- Produces (added to `Looper`):
  ```cpp
  void clearTrack(int t); void clearAll();
  void setOn(int t, bool on); void toggleOn(int t); bool isOn(int t) const;
  void setVolumeDb(int t, float db); float volumeDb(int t) const;     // clamped to [-60, +6]
  void setDryThru(bool on); void setBypass(bool on);
  void snapGains();   // jump every smoothed gain to its target (first run after load)
  ```

**Algorithm:**
- **Recording on an Empty track while L is set** is an overdub onto silence that becomes the track's base. A *zero op* (a pending op that sets `buf[p]=0`) starts at the back-dated start. It is applied before reading or writing at each position for one full pass, so reused memory reads as silence. Positions already passed are zeroed at the press. On close, apply the 5 ms punch-out fade (no tail; L is fixed).
- **Handover.** `pressRec(t)` while another track u is Recording or Overdubbing first does u's close or punch-out (as `pressRec(u)` would), then t's own B action.
  - Over u's tail (first recording) or its 5 ms punch-out, the input splits `g·x` to u and `(1−g)·x` to t. t uses that complementary envelope instead of its own fade-in.
  - Before L is set, the close sets L and t starts at position 0.
- **Only one track records at a time.**
- **Gains.** Per-track target = `on ? db2lin(volume) : 0`. Bypass target 0 for all loops. Dry target = `dry_thru ? 1 : 0` (bypass does not change dry). Smooth every gain linearly over `kRampMs`.
- **Output.** `out = softclip(dryGain*x + Σ trackGain_t * read_t)`, where `softclip(y) = |y| < k ? y : sign(y)*(k + (1−k)*tanh((|y|−k)/(1−k)))` and k = `kSoftClipKnee`.
- **Clear.** `clearTrack` drops the track's layers and redo; the track goes Empty with its gain ramping out, and its buffer is left for the next recording's zero op. When the last non-empty track becomes Empty (clear or base undo), let the ramp finish, then reset L and discard all redo.
- An off track can still be recorded or overdubbed.
- **Disabled engine** (`init` failed): `process()` outputs `dryGain*x` only, and every control call is ignored.

- [ ] **Step 1: Write the failing tests** `tests/test_tracks.cpp`:
  ```cpp
  static void secondTrackLockedToLoop() {
      Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.0f); r.silence(5000);
      r.lp.pressRec(1); CHECK(r.lp.state(1) == ouro::TrackState::Recording);
      r.feed(sine(1000, 0.3f, 480)); r.silence(16000); r.lp.pressRec(1);
      CHECK(r.lp.state(1) == ouro::TrackState::Playing && r.lp.loopLength() == 24064);
      auto a = r.loopImage(); r.silence(10 * 24064); CHECK(maxAbsDiff(a, r.loopImage()) < 1e-6f);
  }
  static void handoverClosesAndStarts() {
      Rig r; r.lp.pressRec(0); r.feed(sine(220, 0.1f, 24064)); r.lp.pressRec(1);
      CHECK(r.lp.state(0) == ouro::TrackState::Playing && r.lp.state(1) == ouro::TrackState::Recording);
      CHECK(r.lp.loopLength() == 24064);
  }
  static void handoverSplitKeepsUnity() {
      Rig r; auto in = sine(220, 0.1f, 48000);                        // tail 50 ms = 2400
      r.lp.pressRec(0); r.feed(slice(in, 0, 24064)); r.lp.pressRec(1);
      r.feed(slice(in, 24064, 24064 + 4864)); r.lp.pressRec(1);
      auto img = r.settle();
      for (uint32_t i = 0; i < 2400; ++i)
          CHECK(std::fabs(img[i] - (in[i] * fadeInGain(i) + in[24064 + i])) < 1e-5f);
  }
  static void onOffRampsAndResumesInPlace() {
      Rig r; r.lp.setTailMs(0); auto base = recordBase(r, 24064, 0.1f);
      r.lp.setOn(0, false); auto o = r.capture(2400);
      CHECK(maxStep(o) < 2.5f * sineStep(220, 0.1f) && peak(slice(o, 1920, 2400)) < 1e-6f);
      r.silence(8000); r.lp.setOn(0, true); r.silence(960);
      uint32_t p = r.lp.position(); auto o2 = r.capture(1000);
      for (uint32_t i = 0; i < 1000; ++i) CHECK(std::fabs(o2[i] - base[(p + i) % 24064]) < 1e-6f);
  }
  static void volumeIsPlaybackOnly() {
      Rig r; r.lp.setTailMs(0); auto base = recordBase(r, 24064, 0.1f);
      r.lp.setVolumeDb(0, -6.0f); auto img = r.settle();
      for (uint32_t i = 0; i < 24064; i += 97) CHECK(std::fabs(img[i] - base[i] * 0.50119f) < 1e-5f);
      r.lp.pressRec(0); r.feed(sine(330, 0.1f, 4800)); r.lp.pressRec(0); r.lp.undo(0);
      CHECK(maxAbsDiff(r.settle(), img) < 1e-5f);
  }
  static void softClipOnlyAboveKnee() {
      // two tracks of amplitude 0.3 (sum 0.6 < knee): exact sum
      // four tracks of amplitude 0.4 at +6 dB: |out| <= 1 everywhere, and samples whose raw sum < knee are unchanged
  }
  static void dryThruAndBypass() {
      Rig r; auto s = sine(220, 0.1f, 4800);
      r.feed(s); CHECK(peak(slice(r.out, 0, 4800)) == 0.0f);
      r.lp.setDryThru(true); r.lp.snapGains(); r.feed(s);
      CHECK(maxAbsDiff(slice(r.out, 4800, 9600), s) < 1e-7f);
      // bypass with a loop playing: loops ramp out, dry stays; un-bypass resumes at position() (as in onOff test)
  }
  static void clearAllResetsAndOverwrites() {
      Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.5f); r.lp.clearAll();
      CHECK(maxStep(r.capture(2400)) < 2.5f * sineStep(220, 0.5f));
      CHECK(r.lp.loopLength() == 0 && r.lp.state(0) == ouro::TrackState::Empty);
      auto q = sine(220, 0.05f, 16000); r.lp.pressRec(0); r.feed(q); r.lp.pressRec(0);
      auto img = r.loopImage();
      for (uint32_t i = 0; i < 16000; ++i) CHECK(std::fabs(img[i] - q[i] * fadeInGain(i)) < 1e-6f);
  }
  static void recordOnEmptyTrackZeroesReusedBuffer() {
      // record t1 loud (amp 0.5), undo t1's base, record t1 again with a 480-sample burst only, mute t0:
      // the image is silent outside [burst start, burst end + 240]
  }
  static void baseUndoRedoAndLengthReset() {
      Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.1f); recordBaseOn(r, 1); auto both = r.settle();
      r.lp.undo(1); CHECK(r.lp.state(1) == ouro::TrackState::Empty && r.lp.canRedo(1));
      r.lp.redo(1); CHECK(maxAbsDiff(r.settle(), both) < 1e-5f);
      r.lp.undo(1); r.lp.undo(0); r.settle();
      CHECK(r.lp.loopLength() == 0 && !r.lp.canRedo(0) && !r.lp.canRedo(1));
  }
  static void offTrackStillRecords() {
      Rig r; r.lp.setTailMs(0); recordBase(r, 24064, 0.0f); r.lp.setOn(1, false);
      r.lp.pressRec(1); r.feed(sine(1000, 0.3f, 4800)); r.lp.pressRec(1);
      CHECK(peak(r.settle()) == 0.0f); r.lp.setOn(1, true); CHECK(peak(r.settle()) > 0.25f);
  }
  static void stressRandomPresses() {                                 // Review Focus 3
      // fixed-seed LCG, 2000 blocks of sine input; each block one random action on a random track among
      // pressRec, undo, redo, toggleOn, clearTrack, clearAll, or nothing. After every block:
      //   every output sample finite and |y| <= 1; pendingOps(t) <= kMaxPendingOps;
      //   loopLength() == 0 exactly when no track is Playing or Overdubbing
  }
  ```
  `recordBaseOn(r, t)` (add it to `looper_rig.hpp`): `pressRec(t)`, feed one pass of `sine(550, 0.1f)`, `pressRec(t)`.
  Also add to `tests/test_budget.cpp` (budget 0.25 MB = 16 blocks; `dub` as in Task 6):
  ```cpp
  static void globallyOldestMerged() {
      Rig r; r.lp.setTailMs(0); r.lp.setBudgetMb(0.25f); recordBase(r, 48000, 0.1f);
      recordBaseOn(r, 1); dub(r, 0, 14400); dub(r, 1, 14400);          // A(t0), B(t1): 4 + 4 blocks
      dub(r, 0, 48000);                                              // C needs 12: A merged
      CHECK(r.lp.undoDepth(0) == 1 && r.lp.undoDepth(1) == 1);
      dub(r, 1, 48000);                                              // D needs 12: B, then C merged
      CHECK(r.lp.undoDepth(0) == 0 && r.lp.undoDepth(1) == 1);
  }
  ```

- [ ] **Step 2: Run them.** `make test` → FAIL.

- [ ] **Step 3: Implement** per the Interfaces and Algorithm.

- [ ] **Step 4: Run them.** `make test` → every engine test `OK`.

- [ ] **Step 5: Commit.** `git add -A && git commit -m "engine: four tracks, handover, on/off, volume, mixing, clear"`

---

### Task 8: Plugin ports and footswitch gestures

**Files:**
- Modify: `OuroborosPlugin.hpp/.cpp`, `Makefile` (momentary TTL patch), `tests/test_plugin.cpp`

**Interfaces:**
- Consumes: the full `Looper` API (Tasks 4–7); `fHmiWc`/`fWidgetDraw` (Task 2).
- Produces: the frozen port list. **Symbols and order are frozen from the first release.**

  | # | `ParamIndex` | symbol | type / range / default |
  |---|---|---|---|
  | 0 | `kParamRec` | `rec` | trigger (`kParameterIsTrigger`) |
  | 1 | `kParamUndoRedo` | `undo_redo` | boolean, momentary (TTL-patched `mod:preferMomentaryOnByDefault`) |
  | 2 | `kParamTrack` | `track` | integer 1–4, enumeration "Track 1"…"Track 4", def 1 |
  | 3 | `kParamOnOff` | `onoff` | trigger |
  | 4 | `kParamVolume` | `volume` | dB −60…6, def 0 (proxy for the selected track) |
  | 5–6 | `kParamUndo`, `kParamRedo` | `undo`, `redo` | trigger |
  | 7–8 | `kParamClearTrack`, `kParamClearAll` | `clear_track`, `clear_all` | trigger |
  | 9–12 | `kParamOn1`…`kParamOn4` | `on_1`…`on_4` | boolean, def 1 |
  | 13–16 | `kParamVolume1`…`kParamVolume4` | `volume_1`…`volume_4` | dB −60…6, def 0 |
  | 17 | `kParamAfterRec` | `after_rec` | enumeration 0 "Rec > Play", 1 "Rec > Dub", def 0 |
  | 18 | `kParamDryThru` | `dry_thru` | boolean, def 0 |
  | 19 | `kParamTail` | `tail` | ms 0–500, def 50 |
  | 20 | `kParamHoldTime` | `hold_time` | ms 200–2000, def 500 |
  | 21 | `kParamBudget` | `budget` | MB 16–400, def 200 |
  | 22 | `kParamOffset` | `offset` | ms 0–30, def 0 (name "Overdub earlier by") |
  | 23 | `kParamTriggerLatency` | `trigger_latency` | ms 0–50, def 3 |
  | 24 | `kParamBypass` | (designation) | `kParameterDesignationBypass` |

  Port groups (template "Port groups" section): `g1_perform` (0–8), `g2_tracks` (9–16), `g3_settings` (17–23).
- Produces: `const ouro::Looper& OuroborosPlugin::engine() const;` and `int OuroborosPlugin::selectedTrack() const;` (0–3), for tests and HMI.

**Behaviour:**
- **Edges.** Edge-detect every trigger/boolean in `run()`, acting on rising edges only.
- **Undo/redo footswitch.** On `undo_redo`, track the hold length in samples. Release before `hold_time` → `undo(sel)` on release. Reaching `hold_time` while held → `redo(sel)` once, and nothing on release.
- **Track selection.** A change of `track` sets the selection and calls `requestParameterValueChange(kParamVolume, volume_N)`.
- **Volume proxy.** A change of `volume` (the proxy) different from `volume_N` sets `volume_N` (internal value and engine) and calls `requestParameterValueChange(kParamVolumeN, v)`.
- **On/off.** A rising `onoff` toggles the selected track and requests `kParamOnN` with the new value.
- **Per-track ports** drive the engine directly. A change on the selected track also requests the proxy (volume only).
- **First run after `activate()`:** apply every value, then `engine.snapGains()`.
- **Activation and memory.** `activate()` calls `engine.reset()`. `sampleRateChanged()` re-runs `engine.init()`; reservation happens in the constructor and here, never in `run()`.
- **Disabled engine:** output = dry when `dry_thru`, else silence (the engine does it, Task 7).
- **Denormals.** Flush denormals at the top of `run()`, copying `boreasFlushDenormals()` from `~/Fun/boreas/plugins/Boreas/BoreasPlugin.cpp`.
- **Makefile.** In `ttl`, patch `mod:preferMomentaryOnByDefault` after `lv2:symbol "undo_redo" ;` (the Boreas `for sym in …` loop).

- [ ] **Step 1: Write the failing tests** in `tests/test_plugin.cpp`. Replace the skeleton test and include `signals.hpp`. Local helpers:
  - `requestStub` records `{index, value}` into `gReqs`; `hasReq(i, v)` searches it.
  - `fresh()` rebuilds the `PluginExporter` (128-frame blocks, 48 kHz); `set(i, v)` = `plugin.setParameterValue(i, v)`.
  - `run(plugin, vec)` processes `vec` into `gOut`; `runN(k)` runs k blocks of `sine(220, 0.1f)`.
  - `recordBaseAndDub()` makes a 24064-sample base on track 0, then a 4800-sample overdub, both via `kParamRec` presses.
  - `eng()` is `static_cast<OuroborosPlugin*>(plugin.getInstancePointer())->engine()`.
  ```cpp
  static void portSymbolsAreFrozen() {
      static const char* kSym[] = { "rec","undo_redo","track","onoff","volume","undo","redo","clear_track",
          "clear_all","on_1","on_2","on_3","on_4","volume_1","volume_2","volume_3","volume_4","after_rec",
          "dry_thru","tail","hold_time","budget","offset","trigger_latency" };
      for (uint32_t i = 0; i < 24; ++i) CHECK(std::strcmp(plugin.getParameterSymbol(i), kSym[i]) == 0);
      CHECK(plugin.getParameterDesignation(kParamBypass) == kParameterDesignationBypass);
  }
  static void recActsOnRisingEdgeOnly() {
      set(kParamRec, 1); runN(1); CHECK(eng().state(0) == ouro::TrackState::Recording);
      set(kParamRec, 0); runN(188); CHECK(eng().state(0) == ouro::TrackState::Recording);
      set(kParamRec, 1); runN(1);   CHECK(eng().state(0) == ouro::TrackState::Playing);
  }
  static void tapUndoHoldRedo() {
      recordBaseAndDub();                      // base + one overdub via kParamRec presses
      set(kParamUndoRedo, 1); runN(37); set(kParamUndoRedo, 0); runN(1);    // ~99 ms tap
      CHECK(eng().undoDepth(0) == 0 && eng().canRedo(0));
      set(kParamUndoRedo, 1); runN(190);                                     // ~507 ms held
      CHECK(eng().undoDepth(0) == 1);                                        // redo fired while held
      set(kParamUndoRedo, 0); runN(1); CHECK(eng().undoDepth(0) == 1);       // no undo on release
  }
  static void trackSelectRetargets() {
      set(kParamTrack, 2); runN(1); set(kParamRec, 1); runN(1);
      CHECK(eng().state(1) == ouro::TrackState::Recording);
  }
  static void volumeProxyFollowsSelectedTrack() {
      set(kParamVolume2, -12); runN(1); gReqs.clear();
      set(kParamTrack, 2); runN(1); CHECK(hasReq(kParamVolume, -12.0f));
      set(kParamVolume, -10); runN(1);
      CHECK(plugin.getParameterValue(kParamVolume2) == -10.0f && eng().volumeDb(1) == -10.0f);
      CHECK(hasReq(kParamVolume2, -10.0f));
  }
  static void onoffFlipsSelectedAndReportsPort() {
      set(kParamTrack, 3); runN(1); gReqs.clear(); set(kParamOnOff, 1); runN(1);
      CHECK(!eng().isOn(2) && hasReq(kParamOn3, 0.0f));
  }
  static void bypassSemantics() {      // replaces the template's test_bypass
      // dry_thru 0: bypass -> silence after 15 ms; dry_thru 1: bypass -> output == input after 15 ms;
      // every toggle: maxStep(out) < 2.5 * sineStep of the probe (sine 220 Hz, amp 0.1)
  }
  static void savedValuesSnapOnLoad() {                               // Review Focus 2
      fresh(); set(kParamDryThru, 1); plugin.activate();
      auto s = sine(220, 0.1f, 128); run(plugin, s); CHECK(maxAbsDiff(gOut, s) < 1e-7f);   // no ramp from 0
  }
  static void reactivateResets() {                                    // Review Focus 1
      recordBaseAndDub(); plugin.deactivate(); plugin.activate(); runN(1);
      CHECK(eng().loopLength() == 0 && eng().state(0) == ouro::TrackState::Empty && eng().pendingOps(0) == 0);
      plugin.deactivate(); plugin.setSampleRate(96000.0, true); plugin.activate();
      set(kParamRec, 1); runN(1); CHECK(eng().state(0) == ouro::TrackState::Recording);
  }
  static void disabledEngineStillPassesDry() {                        // Review Focus 4
      // construct with d_nextSampleRate = 1e12 (reservation fails), dry_thru 1: output == input; rec press ignored
  }
  ```

- [ ] **Step 2: Run them.** `make test` → FAIL (`kParamRec` undefined).

- [ ] **Step 3: Implement** the ports, groups and behaviour above, plus the Makefile momentary patch.

- [ ] **Step 4: Verify.**
  - `make test` → all `OK`.
  - `make && grep -A1 'lv2:symbol "undo_redo"' bin/ouroboros.lv2/ouroboros.ttl` shows `mod:preferMomentaryOnByDefault`.
  - `grep -o 'pg:group <[^>]*>' bin/ouroboros.lv2/ouroboros.ttl | sort | uniq -c` → 9 / 8 / 7.
  - `make test-lv2` → `OK`.

- [ ] **Step 5: Commit.** `git add -A && git commit -m "plugin: frozen port list, footswitch gestures, selected-track proxies"`

---

### Task 9: HmiBridge — labels and LEDs

**Files:**
- Create: `plugins/Ouroboros/hmi/HmiBridge.hpp`, `tests/test_hmi.cpp`
- Modify: `OuroborosPlugin.hpp/.cpp` (wire `hmiAddressed`/`hmiUnaddressed`, call `update` at the end of `run()`), `tests/lv2/test_lv2_hooks.cpp`

**Interfaces:**
- Consumes: `lv2-hmi.h`; `engine()`, `selectedTrack()` (Task 8); `paramFromLv2Index` (Task 2).
- Produces:
  ```cpp
  namespace ouro {
  enum HmiPort : uint8_t { kHmiRec, kHmiUndo, kHmiOnOff, kHmiVolume, kHmiPortCount };
  struct HmiView { TrackState state; NextAction next; bool canUndo; int selected; bool on; };
  class HmiBridge {
  public:
      void setFeature(const LV2_HMI_WidgetControl* wc);                 // nullptr = host without HMI
      void addressed(HmiPort p, LV2_HMI_Addressing a, uint32_t caps);   // non-RT thread; caps = info->caps
      void unaddressed(HmiPort p);                                      // non-RT thread
      void update(const HmiView& v);                                    // run(): sends only what changed
  };
  }
  ```

**Behaviour:**
- **What each port shows:**
  - `kHmiRec`: label `REC`/`PLAY`/`DUB` (next action), and LED by state: Empty White solid, Recording Red blink, Overdubbing Green blink, Playing Green solid. Use `set_led_with_blink(colour, LV2_HMI_LED_Blink_Mid, LV2_HMI_LED_Blink_Mid)` for blink and `LV2_HMI_LED_Blink_None` for solid.
  - `kHmiUndo`: label `UNDO`, or `-` when `!canUndo`. The Dwarf font is ASCII, so not `—` as the spec wrote.
  - `kHmiOnOff`: label `On N`, value `ON`/`OFF`.
  - `kHmiVolume`: label `Vol N`.
- Only call a function whose capability bit is in `caps`.
- `addressed` stores the handle and caps in atomics and marks the port dirty, so the next `update` re-sends everything for it.
- Ports not in the table (`track` etc.) are ignored.

- [ ] **Step 1: Write the failing tests** `tests/test_hmi.cpp`. A fake `LV2_HMI_WidgetControl` records `{fn, addressing, text, colour, on, off}` per call.
  ```cpp
  static void labelsSentOnlyOnChange() {
      ouro::HmiBridge b; b.setFeature(&gFake);
      b.addressed(ouro::kHmiRec, A1, kAllCaps);
      b.update(view(ouro::TrackState::Empty, ouro::NextAction::Rec));
      CHECK(lastLabel(A1) == "REC" && lastLed(A1).colour == LV2_HMI_LED_Colour_White && lastLed(A1).on == 0);
      size_t n = gCalls.size(); b.update(view(ouro::TrackState::Empty, ouro::NextAction::Rec));
      CHECK(gCalls.size() == n);
      b.update(view(ouro::TrackState::Recording, ouro::NextAction::Play));
      CHECK(lastLabel(A1) == "PLAY" && lastLed(A1).colour == LV2_HMI_LED_Colour_Red && lastLed(A1).on == LV2_HMI_LED_Blink_Mid);
  }
  static void undoLabelShowsAvailability() { /* canUndo false -> "-", true -> "UNDO" */ }
  static void knobLabelsFollowTrack() {
      // addressed OnOff (A2) and Volume (A3); view selected = 1, on = false
      // -> label(A2) "On 2", value(A2) "OFF", label(A3) "Vol 2"
  }
  static void readdressResends() { /* unaddressed then addressed again -> the full set is sent again */ }
  static void capsRespected() { /* caps = Label only -> no LED calls */ }
  static void noFeatureNoCalls() {                                    // Review Focus 5
      ouro::HmiBridge b; b.setFeature(nullptr); b.addressed(ouro::kHmiRec, A1, kAllCaps);
      b.update(view(ouro::TrackState::Recording, ouro::NextAction::Play)); CHECK(gCalls.empty());
  }
  ```
  Extend `tests/lv2/test_lv2_hooks.cpp` with `addressedRecGetsLabel`. Find `rec`'s LV2 index by parsing `lv2:index` for `lv2:symbol "rec"` in the bundle TTL; it must equal `2`. Call `addressed(h, idx, A1, &info)`, run one block, and check that the fake saw `set_label(A1, "REC")`.

- [ ] **Step 2: Run them.** `make test` and `make test-lv2` → FAIL.

- [ ] **Step 3: Implement** `HmiBridge.hpp` and wire it into the plugin. `hmiAddressed` maps `paramFromLv2Index` → `HmiPort` (`rec`, `undo_redo`, `onoff`, `volume`).

- [ ] **Step 4: Run them.** Both → `OK`.

- [ ] **Step 5: Commit.** `git add -A && git commit -m "hmi: footswitch and knob labels/LEDs via the MOD HMI extension"`

---

### Task 10: LoopWidget and draw scheduling

**Files:**
- Create: `plugins/Ouroboros/hmi/Font.hpp`, `plugins/Ouroboros/hmi/LoopWidget.hpp`, `tests/test_widget.cpp`
- Modify: `OuroborosPlugin.hpp/.cpp` (widget slots, draw from `run()`), `tests/lv2/test_lv2_hooks.cpp`

**Interfaces:**
- Consumes: `engine()`, `selectedTrack()`; `fWidgetDraw`, `widgetShown`/`widgetHidden` (Task 2).
- Produces:
  ```cpp
  namespace ouro {
  struct WidgetView { int selected; TrackState state[kTracks]; bool on[kTracks];
                      uint32_t loopLength, position, recordedSamples; double sampleRate; bool blinkOn; };
  // Mono1: rows top->bottom, (w+7)/8 bytes per row, MSB = leftmost pixel, 1 = lit. Returns bytes written.
  size_t renderLoopWidget(const WidgetView& v, uint16_t w, uint16_t h, uint8_t* bits);
  void   drawText(uint8_t* bits, uint16_t w, int x, int y, const char* s);    // 5x7 glyphs, 1 px spacing (Font.hpp)
  }
  ```

**Layout:**
- **Tall areas (h ≥ 20):**
  - Text row at y 0: `T<n>` and the selected track's *state*: `--` Empty, `REC`, `DUB`, `PLAY`. This complements the footswitch label, which shows the next action.
  - Marks row at y 10: four 7×7 marks spaced `w/4`. Filled box = has content and on, outline = has content and off, 3×3 centre dot = empty. A Recording/Overdubbing track's mark is drawn only when `blinkOn`.
  - Bar: bottom 7 rows, a 1-px outline with the inside filled to `round((w−2)*position/loopLength)`.
  - When `loopLength == 0` and a track is Recording, the bar area shows `m:ss` of `recordedSamples` instead. When `loopLength == 0` and nothing is recording, the bar is empty.
- **Short areas (h < 20):** the bar alone, using the full height.
- The glyphs needed: `0-9 : - T R E C D U B P L A Y` and space.

**Plugin scheduling:**
- Up to 4 widget slots, each `{std::atomic<ALABS_HMI_Widget> handle; uint16_t w, h; uint8_t last[1024]; bool fresh;}`. `widgetShown` writes w/h/fresh first, then publishes the handle (release).
- Every 960 samples, `run()` renders each shown slot. It calls `draw_bitmap(handle, 0, 0, w, h, bits)` if the bitmap differs from `last` or the slot is fresh, then copies to `last`.
- `blinkOn` toggles every 250 ms of processed audio.
- `widgetHidden` stores a null handle.
- Unknown widget symbols are ignored.

- [ ] **Step 1: Write the failing tests** `tests/test_widget.cpp`. Helper `px(bits, w, x, y)` reads a pixel.
  ```cpp
  static void sizeIsMono1() { uint8_t b[1024]; CHECK(ouro::renderLoopWidget(view(), 40, 37, b) == 5 * 37); }
  static void barFillsProportionally() {
      auto v = view(); v.loopLength = 1000; v.position = 500; uint8_t b[1024]; ouro::renderLoopWidget(v, 40, 37, b);
      int lit = 0; for (int x = 1; x < 39; ++x) lit += px(b, 40, x, 33);    // a row inside the bar
      CHECK(std::abs(lit - 19) <= 1);
  }
  static void emptyBarWhenNoLoop() { /* loopLength 0, no track recording -> no lit pixel inside the bar */ }
  static void elapsedShownWhileRecording() {
      // loopLength 0, state[0] Recording, recordedSamples 7*48000 -> the bar area equals drawText(...,"0:07") at its position;
      // 8 s renders differently
  }
  static void marksReflectTracks() {
      // state {Playing, Playing, Empty, Recording}, on {1, 0, 1, 1}, blinkOn false:
      // mark 0 all 49 pixels lit; mark 1 only its outline (24 px); mark 2 only the centre 3x3; mark 3 nothing
  }
  static void shortAreaBarOnly() { /* 51x8: rows 0..7 form the bar; no text pixels */ }
  ```
  Extend `tests/lv2/test_lv2_hooks.cpp` with `widgetRedrawsWhenProgressMoves`. Record a 0.5 s loop through the `rec` port, show the widget at 40×37, run 2 s, and check that `draw_bitmap` was called at least 3 times with non-null bits, all with rect `(0, 0, 40, 37)`.

- [ ] **Step 2: Run them.** `make test` and `make test-lv2` → FAIL.

- [ ] **Step 3: Implement** `Font.hpp`, `LoopWidget.hpp` and the plugin scheduling. Remove Task 2's clear-only drawing.

- [ ] **Step 4: Run them.** Both → `OK`.

- [ ] **Step 5: Commit.** `git add -A && git commit -m "hmi: loop widget (progress bar, track marks, elapsed time)"`

---

### Task 11: Plain modgui, manual, README

**Files:**
- Modify: `plugins/Ouroboros/modgui.ttl`, `modgui/icon-ouroboros.html`, `modgui/stylesheet-ouroboros.css`, `modgui/script-ouroboros.js`, `modgui/screenshot-ouroboros.png`, `modgui/thumbnail-ouroboros.png`, `docs/manual/ouroboros-manual.html`, `docs/manual/ouroboros-manual.pdf`, `README.md`

**Content:**
- **Face** (template skin, no custom art, a non-goal for v1): knobs `TRACK` (`track`) and `VOLUME` (`volume`), and buttons `REC` (`rec`), `UNDO` (`undo`), `REDO` (`redo`), `ON/OFF` (`onoff`), `CLEAR` (`clear_track`), `CLEAR ALL` (`clear_all`).
  - `undo_redo` is not on the face: the footswitch only, so the web button can't stick.
  - Take button markup from `~/Fun/boreas/plugins/Boreas/modgui/icon-boreas.html` (trigger buttons).
  - `modgui.ttl` lists exactly these ports.
  - Regenerate the screenshot and thumbnail with `modgui/render_screenshot.py`. They are required, or MOD Desktop crashes.
- **Manual:** one page, a musician's tone (template "Writing rules"). It covers the control layout table and the B/C behaviour tables from the spec, loops-only output with a parallel dry path, the widget placement in Starless' Display dialog, and the settings with defaults. Render it with `make manual`.
- **README:** a short description and install steps, as in Boreas' README.

- [ ] **Step 1: Write the face, manual and README.**
- [ ] **Step 2: Verify.**
  - `make` succeeds.
  - Every `mod-port-symbol` in `icon-ouroboros.html` appears in `modgui.ttl`: `grep -o 'mod-port-symbol="[a-z_]*"' plugins/Ouroboros/modgui/icon-ouroboros.html | sort -u` versus `grep -o 'lv2:symbol "[a-z_]*"' plugins/Ouroboros/modgui.ttl | sort -u`.
  - `./install.sh`, then MOD Desktop starts and the pedal renders.
- [ ] **Step 3: Commit.** `git add -A && git commit -m "modgui face, manual, README"`

---

### Task 12: Dwarf build and on-device verification

Needs the Dwarf at `192.168.51.1` running Starless 19, which the user installs first. Do this with the user present: listening is the test.

**Files:**
- Modify (only if tuning changes defaults): `OuroborosPlugin.hpp` parameter defaults.

- [ ] **Step 1: Build and deploy the beta.** `make BETA=1 dwarf`. Expected: `build/dwarf/ouroboros-beta.lv2` is scp'd and jack2/mod-ui restart. Hard-refresh mod-ui.
- [ ] **Step 2: Bind controls.** Load a pedalboard with *Ouroboros (Beta)* and a parallel dry path. Bind B = `rec`, C = `undo_redo` (momentary), knob 1 = `track`, knob 2 = `onoff`, knob 3 = `volume`. Place the *Loop* widget above knob 1 in Device › Display.
- [ ] **Step 3: Checklist** (each item pass/fail, noted in the commit message):
  1. B label goes REC → PLAY → DUB with the LP3 LED colours.
  2. Loop point click-free with a sustained chord, and with a re-struck downbeat at tail 50 ms.
  3. Punch in/out click-free.
  4. Tap C undoes; hold ≥ 0.5 s redoes.
  5. Knob 2 flips the selected track with "On N" shown.
  6. The widget bar fills once per pass, and the marks reflect the tracks.
  7. Handover B on track 2 while track 1 records.
  8. CPU in the mod-ui meter at 128 and 64 frames, compared with LP3 in the same board; no xruns.
  9. Four tracks of 2-minute loops with ~20 overdubs: RAM via `ssh root@192.168.51.1 free -m` stays below the budget.
- [ ] **Step 4: Tune by ear** `tail`, `trigger_latency` and `hold_time`. If a default changes, edit `ranges.def` only (symbols and order untouched), run `make test`, and commit `"tune defaults on device"`.

---

### Task 13: mod-ui patch — plugin-requested changes reach the Dwarf controller

Separate from the plugin repo. Needs the Dwarf on Starless 19.

**Files:**
- Device: `/usr/lib/python3.4/site-packages/mod/host.py`
- Local: `~/Fun/mod/stls19-modui/host.py.orig`, `~/Fun/mod/stls19-modui/host.py`, `~/Fun/mod/stls19-modui/plugin-param-hmi.patch`

**Change:** in `Host.process_read_message_body`, branch `cmd == "param_set"`, final `else:` (after `pluginData['ports'][portsymbol] = value`), add:
```python
                    if instance_id != PEDALBOARD_INSTANCE_ID and portsymbol in pluginData['addressings']:
                        self.paramhmi_set(instance, portsymbol, value, None)
```
`paramhmi_set` is the same path the browser uses (`webserver.py` → `host.paramhmi_set`). It handles enumerations and groups. Python 3.4 compatible.

- [ ] **Step 1: Fetch the device's file** (it is the Starless lineage, not `./mod-ui`):
  ```bash
  mkdir -p ~/Fun/mod/stls19-modui
  scp -O root@192.168.51.1:/usr/lib/python3.4/site-packages/mod/host.py ~/Fun/mod/stls19-modui/host.py.orig
  cp ~/Fun/mod/stls19-modui/host.py.orig ~/Fun/mod/stls19-modui/host.py
  ```
- [ ] **Step 2: Apply the change.** `python3 -m py_compile ~/Fun/mod/stls19-modui/host.py` → no output. Then `diff -u host.py.orig host.py > plugin-param-hmi.patch` (2 added lines).
- [ ] **Step 3: Deploy with a backup:**
  ```bash
  scp -O ~/Fun/mod/stls19-modui/host.py root@192.168.51.1:/tmp/host.py
  ssh root@192.168.51.1 'mount -o remount,rw / && mkdir -p /data/ouroboros-modui-backup && \
    cp -an /usr/lib/python3.4/site-packages/mod/host.py /data/ouroboros-modui-backup/ && \
    install -m664 /tmp/host.py /usr/lib/python3.4/site-packages/mod/host.py && \
    rm -f /usr/lib/python3.4/site-packages/mod/__pycache__/host.*.pyc && systemctl restart mod-ui'
  ```
  Rollback: copy `/data/ouroboros-modui-backup/host.py` back the same way.
- [ ] **Step 4: Verify on the device.**
  - Bind `volume` to knob 3. Set track 2's volume to −12 in the web UI, switch knob 1 to track 2, and turn knob 3 one step clockwise. The display must step from −12 dB, not from track 1's value.
  - Turn a knob bound to another plugin. It must still move one step per detent, which shows no echo loop.
- [ ] **Step 5: Record it.** Copy the patch into the plugin repo as `docs/mod-ui-plugin-param-hmi.patch` and commit (`"docs: mod-ui patch that forwards plugin-requested knob changes to the HMI"`). Sending it upstream to `sejerpz/mod-ui` is the user's decision; don't open a PR.
