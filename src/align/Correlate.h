#pragma once
#include "model/Project.h"
#include <functional>
#include <optional>
#include <vector>

namespace aligna
{
struct OffsetResult
{
    SamplePos lag;
    float confidence;
    bool polarityInverted;
};

// Below this confidence an alignment should not be applied without the user confirming.
constexpr float kConfidentThreshold = 2.0f;

// Finds `lag` such that target[n] ≈ ±reference[n - lag], searching |lag| <= maxLag.
// Coarse GCC-PHAT at ~8 kHz, refined at full rate with normalised cross-correlation.
// progress(0..1) is called repeatedly; returning false cancels (nullopt, error "Cancelled"). It is asked between
// the transforms and every few thousand samples inside the other passes, and never again once it has returned false.
std::optional<OffsetResult> findOffset(const std::vector<float>& reference, const std::vector<float>& target,
                                       SamplePos maxLag, double sampleRate,
                                       const std::function<bool(float)>& progress, juce::String& error);

// As findOffset, but target[0] sits at reference index `targetIndexOffset` (may be negative), i.e. the
// target vector covers reference positions [targetIndexOffset, targetIndexOffset + target.size()).
// The returned lag is in reference positions: target(p) ≈ ±reference(p - lag), |lag| <= maxLag.
std::optional<OffsetResult> findOffset(const std::vector<float>& reference, const std::vector<float>& target,
                                       SamplePos targetIndexOffset, SamplePos maxLag, double sampleRate,
                                       const std::function<bool(float)>& progress, juce::String& error);
} // namespace aligna
