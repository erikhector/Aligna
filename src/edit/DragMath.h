#pragma once
#include "model/Project.h"
#include <vector>

namespace aligna
{
// Grid step used for snapping/nudging in the UI. subdivisionsPerBeat >= 1 -> gridStep(); 0 -> one whole bar.
double snapStep(const Project&, int subdivisionsPerBeat);

struct DragClip
{
    SamplePos start = 0;
    int trackIndex = 0;
};

struct MoveDelta
{
    SamplePos time = 0;
    int tracks = 0;
};

// Multi-clip move. The anchor clip's new start (start + rawTimeDelta) is snapped to `step` when snapEnabled
// (step <= 0 disables), the resulting delta is applied to every clip, then limited so no clip starts before 0 and
// no clip leaves [0, numTracks). rawTrackDelta is limited the same way.
MoveDelta computeMoveDelta(const std::vector<DragClip>& clips, size_t anchorIndex, SamplePos rawTimeDelta,
                           int rawTrackDelta, int numTracks, bool snapEnabled, double step);

// Limits a nudge so that no clip starts before 0.
SamplePos limitNudge(const std::vector<SamplePos>& starts, SamplePos delta);

// Snaps a trimmed edge position when enabled.
SamplePos snapEdge(SamplePos pos, bool snapEnabled, double step);

// True when both projects have identical tracks (by position) and identical clips (all fields, same order).
bool sameClips(const Project& a, const Project& b);
} // namespace aligna
