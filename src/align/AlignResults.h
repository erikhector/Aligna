#pragma once
#include "align/AlignClips.h"
#include "model/Project.h"
#include <optional>
#include <utility>
#include <vector>

namespace aligna
{
// A clip's placement when Align was pressed; used to detect that a result no longer fits the project.
struct ClipAnchor
{
    juce::String clipId;
    bool exists = false; // false: the clip was not found at capture time
    int trackIndex = -1;
    SamplePos start = 0, sourceOffset = 0, length = 0;
};
ClipAnchor captureAnchor(const Project&, const juce::String& clipId);

// One result row of the auto-align dialog (one target clip).
struct AlignRow
{
    juce::String clipId;
    juce::String label;                  // e.g. "Vocals #2"
    SamplePos oldStart = 0;
    std::optional<ClipAlignment> result; // empty when error is set
    juce::String error;
    ClipAnchor anchor;                   // target clip placement when Align was pressed
};

// True if the reference or this row's target no longer matches its anchor (moved, trimmed, re-tracked,
// deleted, or never found): the result's absolute start cannot be applied safely.
bool isStale(const Project&, const ClipAnchor& reference, const AlignRow&);

bool shouldPreTick(const AlignRow&);                          // confident and no error
SamplePos offsetSamples(const AlignRow&);                     // newStart - oldStart (0 without a result)
juce::String formatOffsetMs(SamplePos samples, double rate);  // signed, 3 decimals, e.g. "+300.000 ms"
juce::String formatOffsetSamples(SamplePos samples);          // signed, e.g. "-14400 smp"
juce::String formatConfidence(float);                         // 1 decimal
juce::String describeRow(const AlignRow&, double rate);       // full one-line text incl. notes / error
bool selectionIsCapped(const juce::Range<SamplePos>& sel, double rate); // longer than 60 s

// Moves each (clipId, newStart) clip on its own track. Clips that no longer exist are skipped.
// Returns true if at least one clip actually moved.
bool applyAlignments(Project&, const std::vector<std::pair<juce::String, SamplePos>>&);
} // namespace aligna
