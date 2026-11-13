#pragma once
#include <functional>
#include <juce_audio_basics/juce_audio_basics.h>
#include <optional>

namespace aligna
{
// Offline pitch-preserving time stretch (Rubber Band). Output length is exactly llround(in.getNumSamples() * ratio),
// same channel count as the input, and time 0 of the input maps to time 0 of the output.
// Returns nullopt if cancelled (progress returned false). ratio is clamped to [0.25, 4.0] by the caller.
// progress receives 0..1. Never call from the audio thread.
std::optional<juce::AudioBuffer<float>> stretchBuffer(const juce::AudioBuffer<float>& in, double sampleRate,
                                                      double ratio, const std::function<bool(float)>& progress = {});
} // namespace aligna
