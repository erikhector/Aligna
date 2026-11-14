# Recording Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Record a take from an audio input over the playing project, landing as a new latency-corrected "Take N" track.

**Architecture:** Pure timing/naming math (`RecordTiming`) and file handling (`TakeFiles`) live in `aligna_core` with unit tests. A `Recorder` streams input from the device callback through `juce::AudioFormatWriter::ThreadedWriter` to a 24-bit WAV. `PlaybackChain` starts playback and capture in the same audio callback, and a loopback test with a fake device proves placement to ±1 sample. `AppController` handles the lifecycle (permissions, folders, undo lock, adding the take, moving takes on first save), and the UI adds the Rec button, a meter, a red progress bar and the Audio Settings additions.

**Tech Stack:** C++20, JUCE 8.0.4, Catch2 v3.7.1, CMake.

**Spec:** `docs/superpowers/specs/2026-11-14-recording-design.md`

**Build / test commands (used throughout):**
- Build tests: `cmake --build build --target aligna_tests -j8`
- Run a tag: `./build/aligna_tests "[record]"`
- Full suite: `ctest --test-dir build --output-on-failure`
- Build app: `cmake --build build --target Aligna -j8`

## Global Constraints

- No allocation, locking (except a `SpinLock` *try*-lock) or file I/O on the audio thread.
- Takes: 24-bit PCM WAV, written at the **device rate**, 1 channel (mono) or 2 (stereo), loaded through `SourcePool::import` (resampled to the project rate once).
- Pre-roll: **1 bar** (default) or **Off**. A bar is `samplesPerBeat(project) * beatsPerBar`, and the pre-roll start is clamped at 0.
- Manual latency offset: default 0 ms, range **−500 to +500 ms**, can be negative. It is per machine (app settings) and never stored in the project.
- Latency `L` = driver input latency + driver output latency (device → project rate) + playback-chain delay + manual offset.
- Placement: `start = recordStart`, `sourceOffset = (recordStart − prerollStart) + L`, `length = recorded − sourceOffset`. A negative `sourceOffset` is clamped to 0, and `start` moves later by the difference.
- A take shorter than **0.1 s**, or one that can't be loaded, is discarded with a message (its file stays on disk).
- Track name "Take N", where N = 1 + the highest existing "Take <integer>" track number. Adding the take is one undo step named **"Record"**.
- Takes folder: saved project → `<project folder>/<project name> Takes/`. Unsaved → `<userApplicationDataDirectory>/Aligna/Unsaved Takes/<YYYY-MM-DD HH-MM-SS>/`, with a fresh folder per new/opened project.
- Nothing deletes take files automatically. The first save of an unsaved project moves **every** file in the session folder and rewrites paths in **every** undo state. Save As of a saved project moves nothing.
- The WAV header is rewritten about once a second while recording (ThreadedWriter flush interval = device rate).
- Shortcut **R**. Button text **"Rec"**. No-input message, exactly: **"No audio input available. Choose one in Audio Settings."**
- macOS: no microphone prompt at launch unless the saved device setup has an input enabled. The bundle has a microphone usage text.

## Review Focus

1. **Recording near the start of the project** (playhead under one bar): the pre-roll clamps to 0 and the take still lands correctly. Test in Task 1 (`prerollStart` clamp) and Task 5 (loopback with `recordStart` = 0.5 bar).
2. **A large negative manual offset** (bigger than the pre-roll, or with pre-roll Off): the clip must move later, not read before the start of the file. Test in Task 1.
3. **The device stops or changes mid-take**: everything captured so far is kept and readable, and the user is told. Test in Task 5 (stop the fake device mid-take).
4. **The writer can't keep up** (slow disk / FIFO full): stop with a message and keep the file. Test in Task 4 (tiny buffer).
5. **Undo "Record", save the untitled project, then redo**: the redone take must point at the moved file. Test in Task 3.

---

### Task 1: Timing and naming math

**Files:**
- Create: `src/edit/RecordTiming.h`, `src/edit/RecordTiming.cpp`
- Modify: `CMakeLists.txt` (add `src/edit/RecordTiming.cpp` to `aligna_core`, `tests/RecordTimingTest.cpp` to `aligna_tests`)
- Test: `tests/RecordTimingTest.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace aligna {
  struct TakePlacement { SamplePos start = 0, sourceOffset = 0, length = 0; };
  constexpr double kMinLatencyOffsetMs = -500.0, kMaxLatencyOffsetMs = 500.0;
  double clampLatencyOffsetMs(double ms);
  SamplePos prerollStart(const Project&, SamplePos recordStart, bool prerollOn);
  struct LatencyInputs { int inputLatency = 0, outputLatency = 0; double chainDelay = 0; // device samples
                         double deviceRate = 48000, projectRate = 48000, manualOffsetMs = 0; };
  SamplePos totalLatency(const LatencyInputs&);               // project samples, rounded to nearest
  std::optional<TakePlacement> placeTake(SamplePos prerollStart, SamplePos recordStart, SamplePos latency,
                                         SamplePos recordedLength, double projectRate); // nullopt if length < 0.1 s
  juce::String nextTakeName(const Project&);
  }
  ```

- [ ] **Step 1: Write the failing tests** (tag `[record]`; project 48 kHz, 120 BPM, 4/4, so one bar = 96000)

```cpp
TEST_CASE("prerollStart", "[record]") {
    Project p; p.sampleRate = 48000; p.bpm = 120;
    REQUIRE(prerollStart(p, 200000, true) == 104000);
    REQUIRE(prerollStart(p, 50000, true) == 0);      // clamped
    REQUIRE(prerollStart(p, 200000, false) == 200000);
    p.beatsPerBar = 3;
    REQUIRE(prerollStart(p, 200000, true) == 200000 - 72000);
}
TEST_CASE("totalLatency converts device to project samples", "[record]") {
    REQUIRE(totalLatency({ 256, 256, 0.0, 44100, 48000, 10.0 }) == 1037); // 512*48000/44100 + 480 = 1037.28
    REQUIRE(totalLatency({ 0, 0, 0.0, 48000, 48000, -500.0 }) == -24000);
}
TEST_CASE("placeTake", "[record]") {
    auto a = placeTake(104000, 200000, 480, 500000, 48000);
    REQUIRE(a); REQUIRE(a->start == 200000); REQUIRE(a->sourceOffset == 96480); REQUIRE(a->length == 403520);
    auto b = placeTake(200000, 200000, -480, 100000, 48000);          // negative offset, pre-roll off
    REQUIRE(b); REQUIRE(b->start == 200480); REQUIRE(b->sourceOffset == 0); REQUIRE(b->length == 100000);
    auto c = placeTake(104000, 200000, -120000, 500000, 48000);       // offset bigger than pre-roll
    REQUIRE(c); REQUIRE(c->start == 224000); REQUIRE(c->sourceOffset == 0); REQUIRE(c->length == 500000);
    REQUIRE_FALSE(placeTake(200000, 200000, 0, 4799, 48000));
    REQUIRE(placeTake(200000, 200000, 0, 4800, 48000));
}
TEST_CASE("nextTakeName", "[record]") {
    Project p;
    REQUIRE(nextTakeName(p) == "Take 1");
    for (auto n : { "Vocals", "Take 1", "Take 3", "Take x", "Take 2b" }) { Track t; t.name = n; p.tracks.push_back(t); }
    REQUIRE(nextTakeName(p) == "Take 4");
}
TEST_CASE("clampLatencyOffsetMs", "[record]") {
    REQUIRE(clampLatencyOffsetMs(-900) == -500.0); REQUIRE(clampLatencyOffsetMs(12.5) == 12.5); REQUIRE(clampLatencyOffsetMs(700) == 500.0);
}
```

- [ ] **Step 2: Run to verify it fails.** Build the tests. Expected: compile error, `edit/RecordTiming.h` not found.
- [ ] **Step 3: Implement the functions in `src/edit/RecordTiming.cpp`.** Use `samplesPerBeat(p)` from `model/Grid.h` for the bar length. In `placeTake`, when `sourceOffset < 0`: `start = recordStart - sourceOffset` and `sourceOffset = 0`. `nextTakeName` only counts names matching `^Take [0-9]+$`.
- [ ] **Step 4: Run `./build/aligna_tests "[record]"`.** Expected: all pass.
- [ ] **Step 5: Commit** `feat: recording timing and take naming math`.

---

### Task 2: Take track op and edit-history lock / rewrite

**Files:**
- Modify: `src/edit/ClipOps.h`, `src/edit/ClipOps.cpp`, `src/edit/EditHistory.h`, `src/edit/EditHistory.cpp`
- Test: `tests/ClipOpsTest.cpp`, `tests/EditHistoryTest.cpp`

**Interfaces:**
- Consumes: `TakePlacement` (Task 1).
- Produces:
  ```cpp
  juce::String addTakeTrack(Project&, const SourceInfo&, const juce::String& trackName, const TakePlacement&); // returns clip id; also adds the source to p.sources like addTrackWithClip does
  // EditHistory:
  void setLocked(bool);  bool isLocked() const;            // while locked, perform/undo/redo return false and change nothing
  void rewriteAllStates(const std::function<void(Project&)>&); // applies to every state; no new step, cur/savedIndex/dirty unchanged, no onChange
  ```

- [ ] **Step 1: Write the failing tests**
  - `addTakeTrack places the clip` in ClipOpsTest: source length 500000. After `addTakeTrack(p, src, "Take 1", {200000, 96480, 403520})`: one new track named "Take 1", and its clip has start 200000, sourceOffset 96480, length 403520, stretch 1.0, gain 0.
  - `locked history refuses edits` in EditHistoryTest: perform one edit, then `setLocked(true)`. `perform(...)` returns false and `current()` is unchanged. `undo()` returns false. `isLocked()` is true. After `setLocked(false)`, `undo()` returns true.
  - `rewriteAllStates touches every state`: perform two "Import" edits that each add a source with path "/tmp/a.wav". Run `rewriteAllStates` to set every path to "/x/a.wav". Then `current()`, the state after `undo()` and the state after `redo()` all have the "/x/a.wav" paths. `isDirty()` is the same before and after the rewrite.
- [ ] **Step 2: Run** `./build/aligna_tests "[clipops],[history]"` (use the tags the existing files use). Expected: compile failure.
- [ ] **Step 3: Implement.** `addTakeTrack` calls `addTrackWithClip` and then sets the three fields on the clip it returns.
- [ ] **Step 4: Run the same tests.** Expected: PASS. Then run the full suite. Expected: PASS.
- [ ] **Step 5: Commit** `feat: take track op, history lock and rewrite`.

---

### Task 3: Take files and folders

**Files:**
- Create: `src/io/TakeFiles.h`, `src/io/TakeFiles.cpp`
- Modify: `CMakeLists.txt` (source + `tests/TakeFilesTest.cpp`)
- Test: `tests/TakeFilesTest.cpp`

**Interfaces:**
- Consumes: `EditHistory::rewriteAllStates` (Task 2).
- Produces:
  ```cpp
  juce::File takesFolderFor(const juce::File& projectFile);          // sibling "<name without ext> Takes"
  juce::File defaultUnsavedTakesRoot();                               // userApplicationDataDirectory/Aligna/Unsaved Takes
  juce::File newSessionTakesFolder(const juce::File& root, juce::Time now); // root/"YYYY-MM-DD HH-MM-SS", " (2)"… if it exists; not created
  juce::File uniqueTakeFile(const juce::File& folder, const juce::String& takeName); // folder/"<takeName>.wav", else "<takeName> (2).wav"…
  // Moves every file in `from` into `to` (created), uniquely named; copies if a move fails.
  // Always returns old→new full paths of everything that now lives in `to`; error non-empty if any file could not be moved or copied.
  std::map<juce::String, juce::String> relocateTakes(const juce::File& from, const juce::File& to, juce::String& error);
  void applyRelocation(Project&, const std::map<juce::String, juce::String>&); // rewrites matching SourceInfo::path
  ```

- [ ] **Step 1: Write the failing tests** (all in a `juce::TemporaryFile`-style scratch directory under `juce::File::getSpecialLocation(tempDirectory)`, removed at the end)
  - `takesFolderFor("/x/Song.aligna") == "/x/Song Takes"`.
  - `newSessionTakesFolder(root, Time(2026,10,14,9,5,7,0,true))` has the name `"2026-11-14 09-05-07"` (JUCE months are 0-based). When that folder exists, the name ends in `" (2)"`.
  - `uniqueTakeFile`: in an empty folder it returns `"Take 1.wav"`. After that file is created it returns `"Take 1 (2).wav"`.
  - `relocateTakes moves files`: `from` has `Take 1.wav` and `Take 2.wav`, and `to` already has a `Take 1.wav`. After the call: error is empty; the map has 2 entries; `to` has `Take 1.wav`, `Take 1 (2).wav` and `Take 2.wav`; `from` is empty; the moved bytes are identical.
  - `applyRelocation` changes only matching sources.
  - **Review focus 5, `undo, save, redo keeps the moved path`:** a history with "Record" adding a source in `from`. Undo, then run `relocateTakes` and `rewriteAllStates(applyRelocation)`. Then redo: the source path is under `to` and the file exists.
- [ ] **Step 2: Run** `./build/aligna_tests "[takes]"`. Expected: compile failure.
- [ ] **Step 3: Implement.** Use `File::moveFileTo`, falling back to `copyFileTo`. Compare paths with `getFullPathName()`.
- [ ] **Step 4: Run the tests.** Expected: PASS.
- [ ] **Step 5: Commit** `feat: take file naming and relocation`.

---

### Task 4: Recorder (audio thread → WAV)

**Files:**
- Create: `src/engine/Recorder.h`, `src/engine/Recorder.cpp`
- Modify: `CMakeLists.txt` (source + `tests/RecorderTest.cpp`)
- Test: `tests/RecorderTest.cpp`

**Interfaces:**
- Produces:
  ```cpp
  class Recorder {
  public:
      explicit Recorder(int bufferSamples = 0); // 0 = 10 s at the device rate given to open()
      ~Recorder();                              // closes if open; owns a juce::TimeSliceThread "Aligna recorder"
      bool open(const juce::File&, double deviceRate, int numChannels, juce::String& error); // message thread; 24-bit WAV, flush interval = deviceRate samples
      void begin() noexcept;                                         // audio thread: capture from the next process() on
      void process(const float* const* in, int numIn, int numSamples) noexcept; // audio thread; always updates the peak
      void deviceStopped() noexcept;                                 // audio thread: stop capturing, mark failed
      struct Result { juce::File file; SamplePos numSamples = 0; double deviceRate = 0; };
      Result close();                                                // message thread
      bool isOpen() const; bool isCapturing() const;
      bool hasFailed() const; juce::String failureMessage() const;  // empty if not failed
      float getAndResetPeak() noexcept;                              // max |sample| since the last call
  };
  ```
  Failure messages (exact copy): overflow → `"Recording stopped: the disk could not keep up."`; device → `"Recording stopped: the audio device stopped or changed."`.
- Approach: the audio thread takes a `juce::SpinLock` with a **try**-lock around the `ThreadedWriter*`, and skips the block when the lock is contended (that only happens during open/close). A missing input channel (`numIn < numChannels`) is fed from a zero buffer pre-allocated in `open()`. A `ThreadedWriter::write` that returns false sets the overflow flag and stops capturing.

- [ ] **Step 1: Write the failing tests** (`[recorder]`)
  - `records mono input exactly`: open at 48000 Hz with 1 channel and call `begin()`. Process 10 blocks of 480 samples of a 0.5-amplitude 440 Hz sine. `close()` gives `numSamples == 4800`, and reading the WAV gives 4800 samples equal to the input within `1e-5`.
  - `stereo takes the first two inputs`: 2 channels; the input has L = +0.25 and R = −0.25. The file has 2 channels with those values.
  - `nothing before begin`: process 5 blocks, then `begin()`, then 5 blocks. The file has 2400 samples.
  - `missing channel is silent`: 2 channels, `numIn = 1`. The right channel of the file is all zero.
  - `peak meter`: after processing a 0.5 sine, `getAndResetPeak()` is within 0.01 of 0.5, and the next call returns 0. This works without `open()`.
  - `header is flushed while open`: process 2 s (96000 samples). Poll for up to 3 s until a `WavAudioFormat` reader on the file reports `lengthInSamples >= 48000`. The recorder is still open throughout.
  - **Review focus 4, `overflow fails but keeps the file`:** `Recorder r(1024)`. One `process` of 8192 samples → `hasFailed()`, `isCapturing()` is false, and the message is the overflow text. `close()` gives a readable file.
  - `deviceStopped fails`: `hasFailed()` and the device message.
- [ ] **Step 2: Run** `./build/aligna_tests "[recorder]"`. Expected: compile failure.
- [ ] **Step 3: Implement `Recorder`** as above.
- [ ] **Step 4: Run the tests.** Expected: PASS.
- [ ] **Step 5: Commit** `feat: streaming take recorder`.

---

### Task 5: Record start in the playback chain + loopback test

**Files:**
- Modify: `src/engine/PlaybackChain.h/.cpp`, `src/engine/PlaybackEngine.h/.cpp`
- Test: `tests/RecordLoopbackTest.cpp` (add to CMake), `tests/PlaybackChainTest.cpp`, `tests/PlaybackEngineTest.cpp`

**Interfaces:**
- Consumes: `Recorder` (Task 4), `totalLatency`, `placeTake`, `prerollStart` (Task 1), `SourcePool::import`.
- Produces:
  ```cpp
  // PlaybackEngine
  void prepareStart(SamplePos p);          // message thread: pause, position = playStart = p
  void startFromAudioThread() noexcept;    // playing = true
  // PlaybackChain
  void setRecorder(Recorder*);             // message thread, before the callback is registered
  void armRecording();                     // message thread; next callback: resampler.flushBuffers(), recorder->begin(), engine.startFromAudioThread()
  bool isArmed() const;
  double getDeviceRate() const; int getInputLatency() const; int getOutputLatency() const; // set in audioDeviceAboutToStart
  double getChainDelay() const;            // device samples, measured in audioDeviceAboutToStart
  static double measureResamplerDelay(double ratio, int blockSize); // device samples from engine sample 0 to its output peak, fresh resampler
  ```
- The callback order per block: handle the armed flag, then `recorder->process(in, numIn, n)`, then `player` renders the output. `audioDeviceStopped` calls `recorder->deviceStopped()` when the recorder is capturing.
- `measureResamplerDelay`: run a fresh `ResamplingAudioSource` (2 channels, given ratio) over a source that outputs 1.0 at sample 0 and zeros afterwards, in blocks of `blockSize`. Return the index of the output peak, refined with parabolic interpolation.

- [ ] **Step 1: Write the failing tests**
  - PlaybackEngineTest `prepareStart then startFromAudioThread plays from p; stop returns to p`.
  - PlaybackChainTest `measureResamplerDelay is small and stable`: at ratio 1.0 it is between 0 and 8. At ratio 48000/44100 it is between 0 and 16. Repeated calls give equal results.
  - RecordLoopbackTest (`[loopback]`), parameterised over device rate ∈ {48000, 44100}, project rate 48000, block 480. Call the chain's callbacks directly (no `AudioDeviceManager`). A `FakeDevice` like the one in `PlaybackChainTest.cpp` reports input latency 100 and output latency 156. The test feeds the chain's left output back as input through a 256-sample delay line. The project has one mono source (a 1.0 impulse at project sample 120000 in 5 s of silence), placed at 0 on one track; build the snapshot the way `MixerTest.cpp` does. Steps: `recordStart = 96000`, `prerollStart(p, 96000, true) == 0`; open the recorder (1 channel, device rate); `engine.prepareStart(0)`; `chain.armRecording()`; run 4 s of blocks; `close()`. Then import the take with `SourcePool::import(file, 48000)`, compute `L = totalLatency({100, 156, chain.getChainDelay(), deviceRate, 48000, 0})` and `placeTake(0, 96000, L, len, 48000)`. **Assert** `start + (peakIndexInTake - sourceOffset) == 120000 ± 1`.
  - **Review focus 1:** the same test with `recordStart = 48000` (half a bar); the pre-roll clamps to 0 and the assertion is the same.
  - **Review focus 3, `device stop mid-take keeps audio`:** after 2 s of blocks, call `chain.audioDeviceStopped()`. Then `recorder.hasFailed()` is true, and `close().numSamples` is between 1.9 and 2.1 s worth of samples, readable.
- [ ] **Step 2: Run** `./build/aligna_tests "[loopback],[engine]"`. Expected: compile failure.
- [ ] **Step 3: Implement.** If the 44.1 kHz case is off by a constant amount because of the import resampler in `SourcePool`, measure that delay the same way and add it to the chain-delay term passed to `totalLatency`. The loopback test decides; don't loosen its tolerance.
- [ ] **Step 4: Run the tests, then the full suite.** Expected: PASS.
- [ ] **Step 5: Commit** `feat: sample-accurate record start in playback chain`.

---

### Task 6: AppController recording lifecycle, settings, permissions

**Files:**
- Create: `src/app/AppSettings.h`, `src/app/AppSettings.cpp` (app target)
- Modify: `src/app/AppController.h/.cpp`, `CMakeLists.txt` (app sources; `juce_add_gui_app` gets `MICROPHONE_PERMISSION_ENABLED TRUE` and `MICROPHONE_PERMISSION_TEXT "Aligna uses audio input to record takes."`)

**Interfaces:**
- Consumes: Tasks 1–5.
- Produces:
  ```cpp
  class AppSettings {                         // juce::ApplicationProperties, file "Aligna.settings"
  public:
      double latencyOffsetMs() const; void setLatencyOffsetMs(double); // clamped
      bool prerollOn() const;         void setPrerollOn(bool);         // default true
      std::unique_ptr<juce::XmlElement> audioDeviceState() const; void setAudioDeviceState(const juce::XmlElement*);
  };
  // AppController additions
  AppSettings& settings();
  bool isRecording() const;
  SamplePos recordingStart() const;           // valid while recording
  float inputPeak();                          // recorder.getAndResetPeak()
  void startRecording(std::function<void(juce::String error)> done); // error empty = started; may be async (permission)
  juce::String stopRecording();               // message for the user, empty if fine
  std::function<void(juce::String message)> onRecordingStopped; // fired when a failure poll stops recording
  ```
- **Startup:** `deviceManager.initialise(savedHasInputs ? 2 : 0, 2, savedXml.get(), true)`, where `savedHasInputs` means the saved XML lists active input channels (check the attribute JUCE writes in `createStateXml`). The destructor saves `deviceManager.createStateXml()`. Call `chain.setRecorder(&recorder)` before `addAudioCallback`.
- **startRecording:** if the current device has no active input channels, call `juce::RuntimePermissions::request(recordAudio, …)`. When granted, enable input channel 0 through `setAudioDeviceSetup`. If there is still no input, or permission is denied → `done("No audio input available. Choose one in Audio Settings.")`. Then:
  1. `recordStart = engine.getPosition()` (works whether playing or not).
  2. Compute `pre = prerollStart(...)`.
  3. Pick the folder: `takesFolderFor(file)` when the project is saved, else the session folder. The session folder is created lazily from `newSessionTakesFolder(defaultUnsavedTakesRoot(), now)` and reset in `newProject`/`openProject`.
  4. Take file: `uniqueTakeFile(folder, nextTakeName(project))`.
  5. `recorder.open(file, chain.getDeviceRate(), min(2, activeInputs))`.
  6. `hist.setLocked(true)`, `engine.prepareStart(pre)`, `chain.armRecording()`.
  7. Start a 250 ms `juce::Timer` that stops with `onRecordingStopped(message)` when `recorder.hasFailed()`, or when free space on the take's volume is under 50 MB (message `"Recording stopped: the disk is almost full."`).
- **stopRecording:** stop the timer; `engine.stop()` (returns to the pre-roll start); `res = recorder.close()`; `hist.setLocked(false)`. Then `sources.import(res.file, projectRate)`; on failure → `"The take could not be read: " + err`. Compute `L` from the chain getters plus `settings().latencyOffsetMs()`, then `placeTake`. If it returns nullopt → `"The take was too short and was not kept."`. Otherwise `hist.perform("Record", addTakeTrack(...))`. Prepend the recorder's failure message, if any, to whatever is returned.
- **saveProject:** when the project was unsaved (`file == File()`) and the session folder exists: `map = relocateTakes(session, takesFolderFor(f), err)`, then `hist.rewriteAllStates(applyRelocation(map))`. If `err` is not empty, return false with it. Then save as today.

- [ ] **Step 1: Implement `AppSettings` and the `AppController` changes above.**
- [ ] **Step 2: Build the app and the full test suite.** Expected: both build, and `ctest --test-dir build --output-on-failure` passes.
- [ ] **Step 3: Commit** `feat: recording lifecycle in AppController`.

---

### Task 7: UI, edit blocking, README

**Files:**
- Create: `src/ui/InputMeter.h/.cpp`, `src/ui/AudioSettingsPanel.h/.cpp` (app target)
- Modify: `src/ui/TransportBar.h/.cpp`, `src/ui/MainComponent.cpp`, `src/ui/TimelineView.cpp`, `README.md`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `AppController` recording API and `settings()` (Task 6).
- **TransportBar:**
  - A red "Rec" `TextButton` (tooltip "Record a new take (R)") with `toggleRecord()`: when not recording, call `app.startRecording`, showing any error in an `AlertWindow`; when recording, call `app.stopRecording()` and show any message it returns.
  - Play/Space and Stop call `toggleRecord()` while recording.
  - A red label `"Recording m:ss"`, where the time is `max(0, pos − recordingStart) / rate`.
  - An `InputMeter` (horizontal bar, peak with ~300 ms decay) fed by `app.inputPeak()` in the existing timer. It is visible while recording or while Audio Settings is open.
  - While recording, disable the BPM field, the time-signature boxes and the Audio Settings button.
  - Wire `app.onRecordingStopped` to show the message.
- **AudioSettingsPanel:** replaces the bare selector in `showAudioSettings`.
  - `AudioDeviceSelectorComponent(app.devices(), 0, 2, 2, 2, false, false, false, false)`, so individual input channels can be picked.
  - Below it, a "Recording" section: "Latency offset (ms)" `Slider` (−500 to 500, step 1, text box) and "Pre-roll" `ComboBox` {"1 bar", "Off"}.
  - Both write to `app.settings()` on change.
- **MainComponent:**
  - `R` (no modifiers) calls `transport.toggleRecord()`.
  - While `app.isRecording()`, `keyPressed` handles only Space and R, and `runFileCommand` ignores everything except Quit. Menu items other than Quit are disabled. `filesDropped` is ignored.
  - `requestQuit` calls `stopRecording()` first, so the take is added before the save question.
- **TimelineView:** while recording, ignore mouse down/drag/wheel-edits and `setPlayhead`. Paint a red bar (alpha 0.6) on a lane below the last track, from `toX(recordingStart())` to `toX(max(recordingStart(), position))`.
- **README:** add to Features ("Record a take over the project…"), add `R | Record / stop recording` to the shortcut table, and add a "Recording a take" how-to covering:
  - choosing the input and checking the meter in Audio Settings;
  - pressing R; the one-bar pre-roll and how to turn it off;
  - the take track and the "Record" undo step;
  - where take files are stored, including Unsaved Takes and that they are never deleted automatically;
  - using direct monitoring on the interface;
  - when and how to set the latency offset (record a click along with a click track and compare).

- [ ] **Step 1: Implement the UI changes above.**
- [ ] **Step 2: Build the app and run the full test suite.** Expected: both pass.
- [ ] **Step 3: Manual check with the `run` skill.** Launch the app under Xvfb with a dummy/null audio device, take a screenshot showing the Rec button and meter, and confirm Rec with no input shows the exact no-input message.
- [ ] **Step 4: Commit** `feat: record button, meter, settings and docs`.
