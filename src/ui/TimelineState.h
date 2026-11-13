#pragma once
#include "model/Project.h"
#include <juce_core/juce_core.h>
#include <optional>
#include <set>

namespace aligna
{
// UI-level selection state shared by the timeline, edit commands and auto-align.
struct TimelineState
{
    std::set<juce::String> selectedClipIds;
    std::optional<juce::Range<SamplePos>> timeSelection; // [start, end) in samples
    juce::String alignReferenceClipId;
    bool snapEnabled = true;
    int subdivisionsPerBeat = 1; // 0 = snap to whole bars, otherwise subdivisions per beat (1 = 1/4 in 4/4)
};
} // namespace aligna
