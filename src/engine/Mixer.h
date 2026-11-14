#pragma once
#include "engine/SourcePool.h"
#include "engine/StretchCache.h"
#include "model/Project.h"
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace aligna
{
// Immutable mixing state, built off the audio thread. Clips of every track are sorted by start.
struct MixSnapshot
{
    Project project;
    std::map<juce::String, AudioData> audio; // keyed by bufferKey(clip)
    // Per track, per clip (same order as project.tracks[t].clips): the clip's buffer (nullptr = silent), resolved by
    // makeSnapshot so the audio thread never builds a key string. If absent (hand-built snapshots), renderMix looks
    // unstretched clips up in `audio` by source id; stretched clips are then silent.
    std::vector<std::vector<const juce::AudioBuffer<float>*>> clipAudio;
    // bufferKey of stretched clips whose render failed (they are silent and will not become ready). Not read by
    // renderMix; lets off-thread users of a snapshot tell "failed" from "still stretching".
    std::set<juce::String> failedStretch;
};

// audio[bufferKey(clip)] = stretch 1 ? pool.get(sourceId) : cache.get(key); not-ready or missing buffers are left out.
std::shared_ptr<const MixSnapshot> makeSnapshot(const Project&, const SourcePool&, const StretchCache&);

// Adds (does not clear) numSamples of stereo mix starting at timeline pos `start` into out[ch][offset...].
// Real-time safe: no allocation, locks or I/O.
void renderMix(const MixSnapshot&, SamplePos start, juce::AudioBuffer<float>& out, int offset, int numSamples);

struct ExportSettings
{
    enum class Format { Wav16, Wav24, Mp3 };
    Format format = Format::Wav24;
    int kbps = 320;      // Mp3 only: 192, 256 or 320
    juce::String title;  // Mp3 only: ID3v2 title
};
juce::String extensionFor(ExportSettings::Format); // ".wav" or ".mp3"

// Renders [0, projectEnd) to a stereo WAV (16/24 bit, project rate) or MP3. Writes `<out>.tmp`, checks it is complete
// on disk, then replaces `out` with it, so an existing `out` survives any failure before that point.
// Returns false on error or cancel; `<out>.tmp` is removed, except when the finished export could not be put in
// place: then it is kept (the error names it) so the render is not lost.
bool exportMix(const MixSnapshot&, const juce::File& out, const ExportSettings&, juce::String& error,
               const std::function<bool(float)>& progress = {});
} // namespace aligna
