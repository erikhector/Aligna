#pragma once
#include "model/Project.h"
#include <vector>

namespace aligna
{
// New stretch ratio for dragging the clip's end edge to newEnd: stretch * newLength / oldLength.
// Not clamped (setClipStretch clamps); a newEnd at or before the start counts as a length of one sample.
double stretchForEdge(const Clip&, SamplePos newEnd);

// Ratio the clip tempo dialog will get for a given source tempo, after the same clamping setClipTempo applies.
// `limited` is set when the clamp changed the ratio.
double clipTempoRatio(const Project&, const Clip&, double sourceBpm, bool* limited = nullptr);

// Tap tempo: BPM = 60 / mean interval of the last (up to) 8 taps; a gap of 2 s or more starts over.
class TapTempo
{
public:
    // Registers a tap at `seconds` (any monotonic clock). Returns the BPM, or 0 until there are two taps.
    double tap(double seconds);
    void reset() { taps.clear(); }

private:
    std::vector<double> taps;
};
} // namespace aligna
