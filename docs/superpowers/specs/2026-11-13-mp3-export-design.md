# Aligna — MP3 export design (short)

## Intent

The owner wants to export the mix as **MP3** as well as WAV, so a finished mix can be
shared or played anywhere. Approved by the owner on 2026-11-13 ("Yes do it").

## Goals

- **Export format choice:** File → Export offers **WAV 16-bit**, **WAV 24-bit** or
  **MP3**. MP3 offers a bitrate of **192, 256 or 320 kbps** (constant bitrate), with
  **320** as the default.
- **Save box:** the save box suggests `.mp3` when MP3 is chosen and `.wav` otherwise.
  The extension is corrected to match the chosen format.
- **Channels and rate:** the MP3 is stereo at the project sample rate when that rate is
  32, 44.1 or 48 kHz. At any other project rate the mix is resampled to 44.1 kHz before
  encoding.
- **Title tag:** the MP3 gets an ID3 title tag containing the project name ("Untitled"
  if the project is unsaved).
- **Unchanged guards:** export never overwrites a project source file, refuses while
  clips are still stretching, runs with progress and can be cancelled, and leaves no
  partial file on failure or cancel. This is the same behaviour as WAV export today.
- **Credit:** the README credits LAME and notes its license.

## Non-goals

VBR, other formats (FLAC, OGG, AAC), MP3 metadata beyond the title, and dithering.

## Anti-goals

- Requiring the user to install anything. JUCE's `LAMEEncoderAudioFormat` is rejected
  because it runs an external `lame` program.
- Any change to playback or to WAV output.

## Constraints

- **Library:** LAME 3.100 (LGPL-2.0-or-later, compatible with AGPL-3.0). It is fetched
  with CMake FetchContent from
  `https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz`, pinned
  with `URL_HASH SHA256=ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e`.
- **Build:** only the encoder sources in `libmp3lame/*.c` are compiled, into a static
  library linked by `aligna_core`. The decoder (mpglib) and the frontend are not built.
  Warnings are suppressed for that library only. A hand-written config header is
  provided, with no autotools. CI must stay green on Windows, macOS and Linux.
- **Existing constraints still apply:** no work on the audio thread, and export runs on
  the existing export thread.

## Approaches

| Approach | Trade-off | Decision |
|---|---|---|
| **Bundle LAME, compiled in** | The reference MP3 encoder with the best quality. Needs a small hand-made CMake build. | **Chosen** |
| JUCE `LAMEEncoderAudioFormat` | Already in JUCE, but needs a `lame` program installed by the user. | Rejected (anti-goal) |
| shine (fixed-point encoder) | Tiny and easy to build, but noticeably worse quality. | Rejected |

## Left to the builder

- The encoder wrapper's shape, e.g. `bool encodeMp3(const juce::AudioBuffer<float>&, double rate, int kbps, const juce::String& title, const juce::File&, juce::String& error, progress)` in `src/io/` or `src/engine/`.
- How `exportMix` is split so the rendered mix can feed either writer.
- The export dialog layout.
- Tests: a round-trip decode with JUCE's built-in MP3 reader (`JUCE_USE_MP3AUDIOFORMAT` is already enabled for import), checking the length within one encoder frame plus the delay, and a 1 kHz tone's pitch.
- The cancel test and the 96 kHz → 44.1 kHz case.
