#pragma once
#include "engine/SourcePool.h"
#include "engine/StretchCache.h"
#include "model/Project.h"
#include <functional>
#include <map>
#include <memory>
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
};

// audio[bufferKey(clip)] = stretch 1 ? pool.get(sourceId) : cache.get(key); not-ready or missing buffers are left out.
std::shared_ptr<const MixSnapshot> makeSnapshot(const Project&, const SourcePool&, const StretchCache&);

// Adds (does not clear) numSamples of stereo mix starting at timeline pos `start` into out[ch][offset...].
// Real-time safe: no allocation, locks or I/O.
void renderMix(const MixSnapshot&, SamplePos start, juce::AudioBuffer<float>& out, int offset, int numSamples);

// Renders [0, projectEnd) to a stereo WAV (16 or 24 bit). Returns false on error or cancel.
bool exportMix(const MixSnapshot&, const juce::File& wav, int bitDepth, juce::String& error,
               const std::function<bool(float)>& progress = {});
} // namespace aligna
