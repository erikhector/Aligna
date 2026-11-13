# Aligna — time-stretch design

## Intent and why

The owner wants to fit tracks together that are at **different tempos and different
lengths**. Today Aligna can only move, cut and trim clips, so a 98 BPM part can never
line up with a 100 BPM track for more than a few bars. This adds **per-clip
time-stretching**: a clip plays faster or slower to fit, **without changing pitch**.

It's still a for-fun project. The feature should feel immediate and sound clean, and
it must not compromise smooth playback.

## Goals

1. **Stretch by dragging.** Hold **Ctrl/Cmd** while dragging a clip's **right edge**.
   The clip's audio stretches to the new length instead of being trimmed. The
   live preview shows the new length. Snap applies to the edge (Alt bypasses it),
   like a normal trim. The result is one undo step, "Stretch".
2. **Stretch by tempo.** Right-click a clip and choose **"Clip tempo…"**. A small
   dialog lets you type the clip's original BPM, or **tap** along (the average of
   the last taps, reset after 2 s without a tap), and has a "Clear" button. Once set,
   the clip plays at the **project tempo**: stretch ratio = clipBpm / projectBpm.
   Changing the project BPM later re-stretches every clip that has a tempo set.
   This is one undo step, "Clip Tempo".
3. Both share one engine. A clip has **either** a manual stretch ratio (from
   dragging) **or** a tempo. Dragging the edge of a tempo clip clears its tempo
   and keeps the resulting ratio. Setting a tempo replaces the manual ratio.
4. **Stretched clips act like any other clip.** Move, split, trim, fade, gain,
   nudge, snap and auto-align all work on the stretched audio. Split halves keep the
   same stretch. Auto-align compares the stretched audio.
5. **Visible state.** A stretched clip shows its ratio in the clip label, e.g.
   "drums.wav ×1.02" or "drums.wav 98→100 BPM". While its audio is being
   (re)stretched it shows a "stretching…" overlay and plays silent until ready.
6. Saved in the project file and restored on open (format version 2; format 1 files
   still open, with all clips unstretched).

## Non-goals (this round)

Warp markers (stretching *inside* a clip between pinned points), automatic tempo
detection, pitch shifting, real-time stretching on the audio thread, stretching
from the left edge, and stretching several selected clips in one drag.

## Anti-goals

- Glitches or extra load on the audio thread. Stretching never runs there.
- Losing precision. Stretched clips must still align and nudge to the exact sample.
- Changing source files on disk.
- Freezing the UI while audio is being stretched.

## Constraints

- **Library: Rubber Band Library** (GPL-2.0-or-later, which can be combined with
  Aligna's AGPL-3.0), fetched with CMake FetchContent at a pinned release tag
  (v4.0.0 or newer, at least 2 weeks old). It's built from its single-file
  amalgamation (`single/RubberBandSingle.cpp`) into `aligna_core`, using its
  built-in FFT and resampler, with no extra system dependencies on any OS.
  README credits it and notes the license.
- Offline (non-real-time) stretcher mode, pitch preserved, formants unchanged.
  Engine (R2 "faster" or R3 "finer"): the builder picks the best-sounding one
  that stretches 1 minute of stereo 48 kHz audio in at most about 5 s on the CI
  Linux runner, and states the choice.
- Stretch ratio (output length ÷ input length) is limited to **[0.25, 4.0]**.
  Clip tempo is limited to [20, 400] BPM. Tempo clips are also clamped to the
  ratio range (with the clamp shown in the dialog).
- Existing global constraints stay: no allocation, locks or I/O on the audio
  thread; int64 sample positions; CI green on Windows, macOS and Linux.

## Approaches considered

| Approach | Trade-off | Decision |
|---|---|---|
| **Rubber Band** | Best quality, used by Ardour, Audacity and LMMS; single-file build; GPL-compatible. Large dependency. | **Chosen** |
| SoundTouch | Small and simple; quality drops beyond about ±10%. | Rejected: tempo differences can be bigger than that |
| Own phase vocoder / WSOLA | Fun, no dependency; would sound worse for a long time. | Rejected |
| **Stretch offline in the background, keep the result in memory** | Smooth playback, auto-align and waveforms just work. Short wait after a change; more memory. | **Chosen** |
| Stretch live during playback | Instant changes; complex, and a risk of glitches on the audio thread. | Rejected: conflicts with the main anti-goal |

## How it fits the existing design

- **Model:** `Clip` gains `double stretch = 1.0` (output ÷ input length) and
  `double sourceBpm = 0` (0 means "no tempo set"). For a tempo clip,
  `stretch` is derived from `sourceBpm / project.bpm` whenever either changes, so
  the stored value is always the one in effect.
- **Positions:** a clip's `sourceOffset` and `length` are measured in **stretched
  samples**: positions inside the stretched version of the source, at the project
  rate. Everything downstream (Mixer, TimelineView, hitTest, alignment, ClipOps)
  keeps working unchanged against the stretched buffer. When a clip's stretch
  changes from r₁ to r₂, `sourceOffset`, `length`, `fadeIn` and `fadeOut` are
  scaled by r₂/r₁ (rounded), so the same musical content stays in the clip and its
  `start` doesn't move.
- **Stretched audio:** a new `StretchCache` (engine, message-thread owned) maps
  `(sourceId, stretch)` to an `AudioData` buffer of the whole source stretched by
  that ratio. Clips with the same source and ratio, e.g. split halves, share one
  buffer. Ratio 1.0 uses the original SourcePool buffer directly. Rendering runs on
  a background thread pool. When a render finishes, the snapshot is rebuilt.
  Entries no clip uses are dropped when the project changes. `MixSnapshot.audio`
  is keyed by clip id or by `(sourceId, stretch)`, the builder's choice, so the
  mixer looks up the right buffer. A clip whose buffer isn't ready is treated
  exactly like a missing source: silent, with the "stretching…" overlay instead of
  "missing".
- **Project file:** format 2 adds the clip keys `stretch` and `sourceBpm`.
  Format 1 files load with defaults. Loading validates the ranges above.

## Decisions from review (Claude's calls; owner may override)

- **When rendering happens:** never during a drag. While you drag, the preview draws
  the clip's existing waveform scaled to the new length. On mouse-up the edit
  commits, the clip shows "stretching…" and plays silent until its new buffer is
  ready. Old-ratio audio is not kept playing, because its timing would be wrong.
- **No disk cache.** Opening a project with stretched clips shows them
  "stretching…" for a moment. The in-memory cache keeps recently used entries
  (least-recently-used first out), so undo/redo usually reuses an already
  rendered buffer instantly.
- **Memory:** the stretched-buffer cache has a soft cap of 2 GB. Entries not used
  by the current project are evicted first. Buffers the current project needs are
  always rendered. A stretched source may not exceed 2 hours (the import limit);
  the ratio is clamped so it can't, and the clamp is shown.
- **Buffer length and rounding:** stretched buffer length = llround(source.length ×
  stretch). `sourceOffset`, `length` and fades stay in stretched samples and are
  rescaled from their current values on each stretch change (drift ≤ 1 sample per
  change, which is acceptable). A helper `stretchedSourceLength(project, clip)`
  replaces raw source length in every ClipOps clamp (trim/extend limits), and the
  mixer's existing read clamping (R6) covers any leftover rounding.
- **Drag-stretch on a trimmed clip:** the ratio changes by new visible length ÷ old
  visible length. `sourceOffset` scales with it, so the visible content stays
  the same and only plays faster or slower.
- **Project tempo:** one BPM for the whole project (no tempo map). Changing it is
  one undo step ("Tempo") that also re-derives the stretch of every tempo clip.
  Ratios clamp to [0.25, 4.0], and a clamped clip's label shows the ratio actually
  used.
- **Sample rates and channels:** SourcePool already holds project-rate audio, so
  stretching works on that. Mono stays mono and stereo stays stereo.
- **Auto-align:** a clip whose stretched buffer isn't ready yet gets the per-row
  error "Clip is still stretching — try again in a moment". Nothing waits or blocks.

## Left to the builder

Rubber Band engine and options (see constraints), the thread pool size, the
memory limit or eviction policy for the cache beyond "drop unused", the tap-tempo
dialog layout, the exact cursor for stretch-drag, and the order of work. Suggested
order: dependency + engine (unit-tested: length, pitch preserved, no time offset)
→ model + file format → cache + snapshot → drag-stretch UI → tempo dialog + project
BPM restretch → README.
