# Time-Stretch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Per-clip, pitch-preserving time-stretch in Aligna. You can stretch a clip by Ctrl/Cmd-dragging its right edge, or by giving it a tempo that follows the project BPM.

**Architecture:** `Clip` gains `stretch` and `sourceBpm`. The clip's positions stay in *stretched* samples. A message-thread-owned `StretchCache` renders whole-source stretched buffers with Rubber Band on a background `juce::ThreadPool`. `makeSnapshot` puts each clip's buffer in `MixSnapshot.audio` under a buffer key. Mixer, auto-align and the timeline then read via that key and otherwise run unchanged.

**Tech Stack:** C++20, JUCE 8.0.4, Rubber Band Library (FetchContent, single-file build), Catch2 v3.7.1.

**Spec:** `docs/superpowers/specs/2026-11-13-time-stretch-design.md`

## Global Constraints

- Rubber Band: GPL-2.0-or-later, pinned release tag ≥ v4.0.0 and at least 2 weeks old. Build it from `single/RubberBandSingle.cpp` into `aligna_core` using its built-in FFT and resampler, with no system dependencies on any OS. Credit it in the README.
- Offline stretcher, pitch preserved, formants unchanged. Use whichever engine (R2/R3) sounds best while stretching 1 min of stereo 48 kHz audio in ≤ about 5 s in Release on Linux. State the choice.
- Stretch ratio (output ÷ input length) must stay within **[0.25, 4.0]**. Clip tempo must stay within **[20, 400] BPM**, and `sourceBpm = 0` means no tempo. A stretched source may be at most **2 hours** long at the project rate; clamp the ratio to guarantee it.
- The stretched buffer is **llround(source.length × stretch)** samples long.
- Ratio 1.0 uses the SourcePool buffer directly; it is never rendered.
- No allocation, locks or I/O on the audio thread. Never stretch on the audio thread. Never freeze the UI. Never modify source files.
- Undo names: **"Stretch"** (drag), **"Clip Tempo"** (dialog), **"Tempo"** (project BPM change, replacing "Set BPM").
- The project file moves to **format 2**, with the clip keys `stretch` and `sourceBpm`. Format 1 files still load, with defaults 1.0 and 0.
- Stretched-buffer cache: soft cap **2 GB** (counted as float samples × 4 bytes), least-recently-used eviction, with entries the current project doesn't need evicted first. Buffers the current project needs are always rendered. No disk cache.
- A clip whose buffer isn't ready is silent, shows a **"stretching…"** overlay, and makes auto-align report **"Clip is still stretching — try again in a moment"**.
- Existing constraints still hold: int64 positions, CI green on Windows/macOS/Linux, warning-free test output, new commits only (never amend).

## Review Focus

1. **Project BPM changes while tempo clips exist:** every tempo clip re-stretches, its start doesn't move, and one undo restores everything. Tested in Task 2.
2. **A stretch drag on an already-trimmed clip:** the visible content stays the same and just plays faster or slower, with `sourceOffset` scaled. Tested in Task 2.
3. **Rapid repeated edits** (drag, undo, redo, drag) while renders are in flight: no crash, no stale buffer shown under the wrong key, and the final state is correct. Tested in Task 3.
4. **A clip extended past the stretched source end after a stretch:** clamped to `stretchedSourceLength`, never reading past the buffer. Tested in Task 2.
5. **A project opened with stretched clips whose source file is missing:** the clip shows "missing" rather than "stretching…" forever, and nothing is queued. Tested in Task 3.

---

## File structure

```
src/engine/Stretcher.h/.cpp     stretchBuffer(): Rubber Band wrapper (pure, offline)
src/engine/StretchCache.h/.cpp  async render + LRU cache keyed by buffer key
src/model/Project.h             Clip.stretch, Clip.sourceBpm; bufferKey(), stretchedSourceLength()
src/edit/ClipOps.h/.cpp         setClipStretch, setClipTempo, setProjectBpm; clamps use stretchedSourceLength
src/io/ProjectFile.cpp          format 2
src/engine/Mixer.cpp, src/align/AlignClips.cpp   look audio up by bufferKey(clip)
src/app/AppController.*         owns StretchCache; requests needed keys; rebuilds snapshot on ready
src/ui/TimelineView.*, src/ui/ClipTempoDialog.*, src/ui/TransportBar.cpp, README.md
tests/StretcherTest.cpp, tests/StretchOpsTest.cpp, tests/StretchCacheTest.cpp
```

---

### Task 1: Rubber Band dependency and `stretchBuffer`

**Files:** Modify `CMakeLists.txt`. Create `src/engine/Stretcher.h/.cpp` and `tests/StretcherTest.cpp`.

**Interfaces:**
- Produces:
  ```cpp
  // Returns nullopt if cancelled (progress returned false). ratio is clamped to [0.25, 4.0] by the caller.
  std::optional<juce::AudioBuffer<float>> stretchBuffer(const juce::AudioBuffer<float>& in, double sampleRate,
        double ratio, const std::function<bool(float)>& progress = {});
  ```
  The output has the same channel count as the input, exactly `llround(in.getNumSamples() * ratio)` samples. Use `RubberBandStretcher` in offline mode with pitch scale 1.0. Run `study()` then `process()`, and pad or trim so the length is exact. Compensate the stretcher's start delay so that time 0 in maps to time 0 out.
- [ ] **Step 1:** Add Rubber Band via FetchContent (pinned tag) and compile `single/RubberBandSingle.cpp` into `aligna_core`. Silence warnings from that one third-party source only, not globally. Configure and build all targets. Expected: builds, and test output stays warning-free.
- [ ] **Step 2:** Write the failing tests at 48 kHz:
  - `"length is exact"`: for ratios {0.25, 0.5, 0.98, 1.02, 2.0, 4.0} on a 1 s stereo noise buffer, `out.getNumSamples() == llround(48000 * ratio)` and there are 2 channels.
  - `"pitch preserved"`: a 1 kHz sine at ratio 1.25 or 0.8 has mean zero-crossing spacing in the middle half of 24 ± 0.5 samples.
  - `"timing preserved"`: a click (a 5 ms burst) at 0.5 s at ratio 2.0 has its output onset (first sample above half its peak) within ±5 ms of 1.0 s.
  - `"cancel"`: a progress callback returning false gives nullopt.
  - `"mono stays mono"`.
  - A `[.benchmark]` hidden test that times 60 s of stereo audio at ratio 1.05 and prints the duration.
- [ ] **Step 3:** Run `./build/aligna_tests "[stretcher]"`. Expected: FAIL (does not compile).
- [ ] **Step 4:** Implement it. Choose the engine using the benchmark (Global Constraints), and record the timing and choice in the report.
- [ ] **Step 5:** Run the tests. Expected: PASS. Then run the full ctest.
- [ ] **Step 6:** Commit `feat: rubber band time-stretch wrapper`.

### Task 2: Model, clip operations, file format

**Files:** Modify `src/model/Project.h`, `src/edit/ClipOps.h/.cpp`, `src/io/ProjectFile.cpp` and `tests/ProjectFileTest.cpp`. Create `tests/StretchOpsTest.cpp`.

**Interfaces:**
- Consumes: the existing `Clip`, `Project`, ClipOps and `loadProject`/`saveProject`.
- Produces:
  ```cpp
  // Clip: double stretch = 1.0; double sourceBpm = 0.0;
  constexpr double kMinStretch = 0.25, kMaxStretch = 4.0;
  juce::String bufferKey(const Clip&);  // sourceId if stretch == 1.0, else sourceId + "@" + String(stretch, 17 sig digits)
  SamplePos stretchedSourceLength(const Project&, const Clip&); // llround(source.length * stretch); 0 if source unknown
  double maxStretchFor(const Project&, const Clip&);            // min(kMaxStretch, 2h*rate / source.length)
  bool setClipStretch(Project&, const juce::String& clipId, double newStretch); // clamps; rescales offset/length/fades; clears sourceBpm; false if unchanged
  bool setClipTempo(Project&, const juce::String& clipId, double sourceBpm);     // 0 clears (stretch stays); else stretch = clamp(bpm / p.bpm); false if unchanged
  bool setProjectBpm(Project&, double bpm);   // clamps [20,400]; re-derives stretch of every clip with sourceBpm > 0 (start unchanged); false if unchanged
  ```
  `setClipStretch` rescales `sourceOffset`, `length`, `fadeIn` and `fadeOut` by r₂/r₁ using llround, then clamps `length` to `stretchedSourceLength − sourceOffset` (at least 1) and re-clamps fades. The start never changes. `setClipTempo` with bpm > 0 uses the same rescale path but keeps `sourceBpm`. Every existing ClipOps clamp that used the raw `source.length` (trimClipEnd, trimClipStart's lower bound if it uses the length, `addTrackWithClip`) now uses `stretchedSourceLength`. `splitClip` copies `stretch` and `sourceBpm` to both halves.
  Format 2 writes `stretch` (a number) and `sourceBpm` (a number) on every clip. When loading format 1 or 2, a missing key means the default. Validate: stretch in [0.25, 4.0], and sourceBpm 0 or in [20, 400]. Errors name the key.
- [ ] **Step 1:** Write the failing tests in `tests/StretchOpsTest.cpp`, using a source of 48000 samples and a clip at start 1000 with length 48000:
  - `setClipStretch(p,id,2.0)` gives length 96000, start 1000 and `bufferKey` ≠ sourceId.
  - Calling it again with 2.0 returns false.
  - 10.0 clamps to 4.0, giving length 192000.
  - A trimmed clip (sourceOffset 12000, length 24000) at ratio 2.0 gives sourceOffset 24000 and length 48000 (Review Focus 2).
  - Fades of 4800/4800 at ratio 0.5 give 2400/2400.
  - `trimClipEnd` past the end after stretch 2.0 is clamped to `stretchedSourceLength − offset` (Review Focus 4).
  - `splitClip` on a stretched tempo clip: both halves keep `stretch` and `sourceBpm`.
  - `setClipTempo(p,id,98)` at project bpm 100 gives `stretch == Approx(0.98)` and length `llround(48000*0.98)`.
  - `setProjectBpm(p,196)` re-derives stretch to 0.5 for the tempo clip, leaves a non-tempo stretched clip unchanged, and keeps start 1000 (Review Focus 1).
  - Doing it through `EditHistory::perform` and undoing restores the original stretch and length.
  - `setClipTempo(p,id,0)` clears sourceBpm and keeps stretch.
  - `maxStretchFor` for a 1 h source at 48 kHz gives 2.0.
- [ ] **Step 2:** In `ProjectFileTest`, add:
  - a format 2 round-trip with stretch 1.5 and sourceBpm 98;
  - a format 1 file that loads with defaults;
  - stretch 5.0 rejected, naming `stretch`;
  - sourceBpm 10 rejected.
- [ ] **Step 3:** Run them. Expected: FAIL.
- [ ] **Step 4:** Implement it. In `TransportBar.cpp:97`, change the BPM commit to `perform("Tempo", [v](Project& p){ return setProjectBpm(p, v); })`.
- [ ] **Step 5:** Run the tests. Expected: PASS. Then run the full ctest and fix any existing test that assumed raw source length.
- [ ] **Step 6:** Commit `feat: clip stretch and tempo in model, clip ops and project file`.

### Task 3: StretchCache, snapshot keys, engine wiring

**Files:**
- Create `src/engine/StretchCache.h/.cpp` and `tests/StretchCacheTest.cpp`.
- Modify `src/engine/Mixer.h/.cpp`, `src/align/AlignClips.cpp`, `src/app/AppController.h/.cpp`, `tests/MixerTest.cpp` and `tests/AlignTest.cpp`.

**Interfaces:**
- Consumes: `stretchBuffer`, `bufferKey`, `SourcePool::get`, `AudioData`.
- Produces:
  ```cpp
  class StretchCache {           // message-thread API; renders on an internal juce::ThreadPool (2 threads)
  public:
    explicit StretchCache(size_t softCapBytes = size_t(2) << 30);
    // Ensure every key the project needs is ready or queued; cancel queued/running jobs no longer needed.
    void require(const Project&, const SourcePool&);
    AudioData get(const juce::String& key) const;      // nullptr if not ready
    bool isPending(const juce::String& key) const;
    std::function<void()> onReady;                     // called on the message thread after any render completes
    void waitForIdle();                                // tests only: block until no jobs are queued/running, then deliver onReady
    size_t bytesUsed() const;
  };
  // makeSnapshot gains the cache: audio[bufferKey(clip)] = stretch == 1 ? pool.get(sourceId) : cache.get(key)
  std::shared_ptr<const MixSnapshot> makeSnapshot(const Project&, const SourcePool&, const StretchCache&);
  ```
  Results come back to the message thread via `juce::MessageManager::callAsync`, guarded by a liveness token so a destroyed cache is never touched. A result whose key is no longer required is still stored, since undo may need it again. Eviction is least-recently-used: unneeded keys go first, and needed keys are never evicted. Sources that are missing or not loaded are never queued (Review Focus 5). `Mixer.cpp:37` and `AlignClips.cpp:23` look up `bufferKey(clip)` instead of `clip.sourceId`. In AlignClips, a stretched clip with no buffer gives the error "Clip is still stretching — try again in a moment". AppController owns the `StretchCache`, calls `require` on every project change, rebuilds the snapshot from `onReady`, and exposes `stretchCache()` for the UI.
- [ ] **Step 1:** Write the failing tests:
  - `"renders required keys"`: a project with one clip at stretch 2.0, then `require` and `waitForIdle()`, makes `get(bufferKey(clip))` non-null with `llround(len*2)` samples, and `onReady` was called. Tests that need the message loop use `juce::ScopedJuceInitialiser_GUI` and run the dispatch loop inside `waitForIdle()`.
  - `"ratio 1 is never rendered"`: `bytesUsed() == 0`.
  - `"missing source not queued"`: `isPending` is false and `get` is null.
  - `"rapid changes"` (Review Focus 3): call `require` for stretch 1.5, 2.0, 1.5, then 3.0 back to back, then `waitForIdle`. `get` for 3.0 is correct, and a key's buffer always has that key's ratio length.
  - `"eviction"`: with a cap of about 1 MB, rendering three unneeded keys evicts the oldest, and the needed key survives.
  - MixerTest: a snapshot with a stretched clip renders the cache buffer. A stretched clip whose buffer isn't ready renders silence.
  - AlignTest: a stretched clip with no buffer gives the "still stretching" error.
- [ ] **Step 2:** Run them. Expected: FAIL.
- [ ] **Step 3:** Implement it.
- [ ] **Step 4:** Run the tests. Expected: PASS. Then run the full ctest.
- [ ] **Step 5:** Commit `feat: background stretch cache wired into mixer and auto-align`.

### Task 4: Stretch UI, clip tempo dialog, README

**Files:**
- Modify `src/ui/TimelineView.h/.cpp`, `src/ui/TransportBar.cpp` and `README.md`.
- Create `src/ui/ClipTempoDialog.h/.cpp` (app target).
- Pure helpers go in `src/edit/DragMath.*` (or a new `src/edit/StretchMath.*`) with tests.

**Interfaces:**
- Consumes: `setClipStretch`, `setClipTempo`, `bufferKey`, `AppController::stretchCache()` (`isPending`, `get`), the existing `Drag`/`DragMode` machinery and `sameClips`.
- Produces / behaviour:
  - **Drag-stretch:** Ctrl/Cmd plus mouse-down on the `TrimEnd` zone starts a new `DragMode::Stretch`. The new edge position uses the same snap/Alt rules as trim. The ratio is `clip.stretch × newLength / oldLength`, from the pure helper `double stretchForEdge(const Clip&, SamplePos newEnd)` (unit-tested). The preview applies `setClipStretch` to the preview project and draws the existing waveform scaled. Mouse-up performs "Stretch" through the existing no-op-skipping commit.
  - **Cursor:** left/right resize during a stretch drag, plus a small "↔ stretch" label near the cursor.
  - **Clip label:** "name ×1.02" when stretch ≠ 1 and there's no tempo; "name 98→100 BPM" for a tempo clip. Use the ratio actually applied, after clamping.
  - **Overlay:** a translucent "stretching…" overlay when `stretchCache().isPending(bufferKey(clip))`. Repaint when `onReady` fires.
  - **Waveform:** thumbnails are keyed by `bufferKey(clip)` and built from the clip's actual buffer. While pending, draw the old thumbnail horizontally scaled, or nothing if there is none.
  - **ClipTempoDialog**, from the right-click item "Clip tempo…":
    - a BPM field (20–400), a big "Tap" button, a "Clear" button, OK and Cancel;
    - tap BPM = 60 / mean interval of the last up to 8 taps, reset after 2 s without a tap (a pure helper `TapTempo` with tests);
    - if the resulting ratio clamps, show "Limited to ×4.00 (or ×0.25)";
    - OK performs "Clip Tempo".
  - **README:** a "Stretching clips" section covering drag-stretch, clip tempo, tap and how it follows the project BPM, plus a Rubber Band credit and license note in the credits/license section.
- [ ] **Step 1:** Write the failing tests for `stretchForEdge` and `TapTempo`:
  - `stretchForEdge`: length 48000 at stretch 1 with newEnd = start + 96000 gives 2.0; a clip already at 2.0 dragged to half its length gives 1.0.
  - `TapTempo`: taps every 0.5 s give 120; a 2.5 s gap resets; 1 tap gives 0 (no tempo).
- [ ] **Step 2:** Run them. Expected: FAIL.
- [ ] **Step 3:** Implement the helpers and the UI.
- [ ] **Step 4:** Run the full ctest. Expected: PASS, warning-free.
- [ ] **Step 5:** GUI check under Xvfb with xdotool (build-notes). A temporary local auto-import patch is fine as long as it's reverted.
  - Ctrl-drag a clip's right edge to about 1.5× its length. Screenshot the "stretching…" overlay and then the stretched waveform with its "×1.50" label.
  - Ctrl+Z restores it.
  - Open "Clip tempo…" and type 100 with project BPM 120. The label reads "100→120 BPM".
  - Change the project BPM to 100. The label shows ×1.00 / "100→100 BPM".
  - Look at every screenshot.
- [ ] **Step 6:** Commit `feat: stretch by drag and by clip tempo`.
