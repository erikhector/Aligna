# Aligna — design

*Working name, Claude's call; easy to change.*

## Intent and why

A hobby, open-source DAW built **for fun**. The owner's main use: **editing
tracks and aligning different tracks to each other**. It ships as a native
executable people run on their own computer (Windows, macOS, Linux).

Because it's for fun, the project should make sound early and grow in small,
satisfying steps. A small, working app beats a large, half-done one.

## Goals (v1)

1. **Import audio** files (WAV, AIFF, FLAC, MP3) by menu or drag-and-drop.
   Each file becomes a clip on its own new track.
2. **Timeline** with stacked tracks, zoomable/scrollable waveforms (zoom down to
   individual samples), a time ruler (bars/beats and min:sec), and a playhead.
3. **Playback** of all tracks mixed together. Transport: play/pause, stop,
   click-to-locate, spacebar toggles play. Per-track volume, pan, mute, solo.
4. **Clip editing (non-destructive):** select, drag to move (also to other
   tracks), trim start/end, split at playhead, delete, per-clip gain,
   short fade-in/out handles. Source files are never modified.
5. **Precise alignment tools — the heart of the app:**
   - **Nudge** selected clips by keyboard: one sample, 1 ms, 10 ms, one grid step.
   - **Beat grid + snapping:** project tempo (BPM) and time signature; clip
     edges snap to grid lines when snap is on (toggle, plus hold a modifier to
     bypass).
   - **Auto-align:** select a reference clip and one or more other clips →
     "Align to reference" finds the time offset by cross-correlation
     (GCC-PHAT via FFT on a mono, downsampled copy, then refined at full
     rate) and moves the clips. Intended for several recordings of the same
     performance. Shows a confidence value; the user can undo.
   - Optional visual help: a vertical guide line follows the cursor across
     all tracks so transients can be compared by eye.
6. **Undo/redo** for every edit.
7. **Save/load projects** as a human-readable JSON file (`.aligna`) that
   references audio files by relative path where possible.
8. **Export** the mix to WAV (16/24-bit, project sample rate).
9. **Audio device settings** dialog (output device, sample rate, buffer size).

## Non-goals (v1)

Recording, MIDI, virtual instruments, effects, VST/AU plugin hosting,
time-stretching/warping, automation, a mixer view beyond per-track controls.
Plugin hosting stays possible later (JUCE supports it); nothing should
make it harder.

## Anti-goals

- Glitchy or drifting playback. If alignment is sample-precise in the
  editor but playback stutters or drifts, the app fails its purpose.
- Destroying or rewriting the user's audio files.
- A build that only works on one developer machine.

## Constraints

- **Language/framework:** C++20 with **JUCE 8** (Claude's pick, agreed by
  owner). CMake build; JUCE fetched with CMake FetchContent at a pinned tag.
- **License:** JUCE 8 is free for open-source under **AGPLv3**, so the
  project is **AGPL-3.0**.
- **Platforms:** Windows, macOS, Linux. GitHub Actions builds all three on
  every push and attaches downloadable executables to tagged releases.
- Real-time safety: no allocation, locks or file I/O on the audio thread.
  Audio files are streamed / cached off the audio thread.

## Approaches considered

| Approach | Trade-off | Decision |
|---|---|---|
| **C++ + JUCE** | Industry standard for audio; best device/file/timing support; plugin hosting later. Steeper language, slower builds. | **Chosen** — sample-precise alignment and smooth playback matter most. |
| Rust (egui, cpal, symphonia) | Modern, pleasant, single binary; less mature audio ecosystem; plugin hosting hard. | Rejected. |
| Tauri + Web Audio | Fastest pretty UI; timing/performance limits hurt precise alignment. | Rejected. |

Within JUCE, Tracktion Engine (a ready-made editing engine) was considered.
**Claude's call: write our own small engine on plain JUCE.** v1's needs are
modest, it keeps the code understandable, and building it is part of the fun.

## Architecture sketch

- **Model:** `Project` → `Track[]` → `Clip[]`. A clip holds source-file id,
  timeline start (in samples, 64-bit), source offset, length, gain, fades.
  Positions stored in samples at project rate; files at other rates are
  resampled on playback.
- Model changes go through an edit/command layer that records undo.
- **Engine:** an `AudioSource` that, per block, reads from each track's clips
  (via buffered readers on a background thread) and mixes. It reads an
  immutable snapshot of the model, swapped atomically on edits.
- **UI:** JUCE components: transport bar, ruler, track headers, track lanes
  with waveform rendering (use JUCE `AudioThumbnail` with a cache).
- **Alignment:** a standalone, unit-tested module (`align/`) with no UI
  dependency.
- **Tests:** Catch2 (or JUCE UnitTest) for model, undo, project file
  round-trip, snapping math and cross-correlation (synthetic signals with
  known offsets).

## Decisions from review (Claude's calls; owner may override)

- **Sample rate:** the project rate is chosen at project creation (default
  48 kHz) and never changes; positions stay exact sample counts at that rate.
  If the device runs at another rate, the engine converts at playback time.
  Each clip reader resamples straight from the file rate to the device rate,
  and the engine maps timeline positions to device positions using doubles.
  The stored model is never rescaled. Export renders at the project rate.
- **Overlapping clips on one track:** allowed. The clip that starts later
  covers the earlier one where they overlap. There's no automatic crossfade,
  and the clips' own fade handles can smooth the join. The covered part is
  drawn dimmed. Moves are never blocked.
- **Auto-align scope:**
  - It only shifts clips and never stretches them, so drift isn't corrected.
  - It compares the overlap inside the timeline selection if there is one,
    otherwise up to the first 60 s of overlap.
  - The default search window is ±30 s, adjustable in the dialog up to ±5 min.
  - It runs on a background thread with progress shown and can be cancelled.
  - It reports inverted polarity as a notice and doesn't flip anything.
- **Releases:** unsigned builds (a macOS .app in a zip, a Windows .exe in a
  zip, a Linux AppImage or tarball). The README explains how to get past the
  Gatekeeper and SmartScreen warnings. Signing can be added later.
- **Audio channels:** clips are mono or stereo and the mix bus is stereo.
  Mono clips are panned with the track's pan control.

## Left to the builder

File/module layout, exact keyboard shortcuts, colours and visual style
(dark theme, clean), test framework choice, the waveform-cache details, and
the order of work. Suggested order: build skeleton + CI → playback of one file
→ multiple tracks → timeline/waveforms → editing + undo → grid/snap/nudge →
auto-align → save/load → export.
