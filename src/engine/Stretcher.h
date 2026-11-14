#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <juce_audio_basics/juce_audio_basics.h>
#include <optional>

namespace aligna
{
// Offline pitch-preserving time stretch (Rubber Band). Output length is exactly llround(in.getNumSamples() * ratio),
// same channel count as the input, and time 0 of the input maps to time 0 of the output.
// Returns nullopt if cancelled (progress returned false), or if a non-empty input's output length would be < 1 or
// would not fit an int. ratio is clamped to [0.25, 4.0] by the caller.
// progress receives 0..1. Never call from the audio thread.
std::optional<juce::AudioBuffer<float>> stretchBuffer(const juce::AudioBuffer<float>& in, double sampleRate,
                                                      double ratio, const std::function<bool(float)>& progress = {});

// Same, for the source samples [srcBegin, srcEnd) of `in` only (clamped to the buffer), read in place.
// The result is laid out on the grid of a whole-buffer render: its length is exactly
// llround(srcEnd * ratio) - llround(srcBegin * ratio), and its sample i belongs at index stretchedIndex(srcBegin,
// ratio) + i of stretchBuffer(in). It is time-aligned with that render (transients within a sample or two) but not
// bit-identical: Rubber Band sees nothing outside the range (bar a lead-in shorter than one analysis hop), so the
// first and last tens of milliseconds differ most, and phases differ throughout. Callers that need clean audio up
// to a point should start the range a little before it.
std::optional<juce::AudioBuffer<float>> stretchRange(const juce::AudioBuffer<float>& in, std::int64_t srcBegin,
                                                     std::int64_t srcEnd, double sampleRate, double ratio,
                                                     const std::function<bool(float)>& progress = {});

// Index, in the stretched source, of the sample that source sample `srcPos` maps to.
inline std::int64_t stretchedIndex(std::int64_t srcPos, double ratio)
{
    return (std::int64_t) std::llround((double) srcPos * ratio);
}
} // namespace aligna
