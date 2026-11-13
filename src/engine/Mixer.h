#pragma once
#include "engine/SourcePool.h"
#include "model/Project.h"
#include <functional>
#include <map>
#include <memory>

namespace aligna
{
// Immutable mixing state, built off the audio thread. Clips of every track are sorted by start.
struct MixSnapshot
{
    Project project;
    std::map<juce::String, AudioData> audio;
};

std::shared_ptr<const MixSnapshot> makeSnapshot(const Project&, const SourcePool&);

// Adds (does not clear) numSamples of stereo mix starting at timeline pos `start` into out[ch][offset...].
// Real-time safe: no allocation, locks or I/O.
void renderMix(const MixSnapshot&, SamplePos start, juce::AudioBuffer<float>& out, int offset, int numSamples);

// Renders [0, projectEnd) to a stereo WAV (16 or 24 bit). Returns false on error or cancel.
bool exportMix(const MixSnapshot&, const juce::File& wav, int bitDepth, juce::String& error,
               const std::function<bool(float)>& progress = {});
} // namespace aligna
