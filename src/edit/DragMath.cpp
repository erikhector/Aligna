#include "edit/DragMath.h"
#include "model/Grid.h"
#include <algorithm>

namespace aligna
{
double snapStep(const Project& p, int subdivisionsPerBeat)
{
    if (subdivisionsPerBeat <= 0)
        return samplesPerBeat(p) * (double) std::max(1, p.beatsPerBar);
    return gridStep(p, subdivisionsPerBeat);
}

SamplePos snapEdge(SamplePos pos, bool snapEnabled, double step)
{
    return snapEnabled ? snap(pos, step) : pos;
}

MoveDelta computeMoveDelta(const std::vector<DragClip>& clips, size_t anchorIndex, SamplePos rawTimeDelta,
                           int rawTrackDelta, int numTracks, bool snapEnabled, double step)
{
    MoveDelta d;
    if (clips.empty() || numTracks <= 0)
        return d;
    const auto& anchor = clips[std::min(anchorIndex, clips.size() - 1)];
    SamplePos time = rawTimeDelta;
    if (snapEnabled && step > 0.0)
        time = snap(std::max<SamplePos>(0, anchor.start + rawTimeDelta), step) - anchor.start;

    SamplePos minStart = clips[0].start;
    int minTrack = clips[0].trackIndex, maxTrack = clips[0].trackIndex;
    for (auto& c : clips)
    {
        minStart = std::min(minStart, c.start);
        minTrack = std::min(minTrack, c.trackIndex);
        maxTrack = std::max(maxTrack, c.trackIndex);
    }
    d.time = std::max(time, -minStart);
    d.tracks = std::clamp(rawTrackDelta, -minTrack, std::max(-minTrack, numTracks - 1 - maxTrack));
    return d;
}

SamplePos limitNudge(const std::vector<SamplePos>& starts, SamplePos delta)
{
    if (starts.empty())
        return 0;
    return std::max(delta, -*std::min_element(starts.begin(), starts.end()));
}
} // namespace aligna
