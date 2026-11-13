#pragma once
#include "align/Correlate.h"
#include "engine/Mixer.h"
#include <juce_core/juce_core.h>

namespace aligna
{
struct ClipAlignment
{
    SamplePos newStart;
    float confidence;
    bool polarityInverted;
};

constexpr double kMaxRegionSeconds = 60.0;
constexpr double kMaxShiftSecondsLimit = 300.0;

// Works out where the target clip should start so that it lines up with the reference clip.
// Region: selection ∩ reference clip if a selection is given, else reference clip ∩ (target clip widened by
// maxShiftSeconds); capped to its first 60 s. Only shifts; never touches the project.
// Runs synchronously (call it from a background thread); progress returning false cancels.
std::optional<ClipAlignment> computeAlignment(const MixSnapshot&, const juce::String& referenceClipId,
                                              const juce::String& targetClipId,
                                              std::optional<juce::Range<SamplePos>> selection, double maxShiftSeconds,
                                              const std::function<bool(float)>& progress, juce::String& error);
} // namespace aligna
