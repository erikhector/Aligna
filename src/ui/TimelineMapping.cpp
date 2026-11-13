#include "ui/TimelineMapping.h"
#include <algorithm>
#include <cmath>

namespace aligna
{
void TimelineMapping::zoomAround(double x, double factor)
{
    const double anchor = (double) viewStart + x * samplesPerPixel;
    samplesPerPixel = std::clamp(samplesPerPixel * factor, minSamplesPerPixel, maxSamplesPerPixel);
    viewStart = std::max<SamplePos>(0, (SamplePos) std::llround(anchor - x * samplesPerPixel));
}

Hit hitTest(const Project& proj, const TimelineMapping& m, int trackHeight, juce::Point<float> p)
{
    Hit hit;
    if (trackHeight <= 0 || p.y < 0.0f)
        return hit;
    const int ti = (int) (p.y / (float) trackHeight);
    if (ti >= (int) proj.tracks.size())
        return hit;
    hit.trackIndex = ti;
    const float ly = p.y - (float) (ti * trackHeight);

    const Clip* best = nullptr;
    for (auto& c : proj.tracks[(size_t) ti].clips)
    {
        const double l = m.toX(c.start), r = m.toX(c.start + c.length);
        if (p.x < l || p.x > r)
            continue;
        if (best == nullptr || c.start >= best->start) // later-starting wins; ties go to the later in order
            best = &c;
    }
    if (best == nullptr)
        return hit;

    hit.clipId = best->id;
    const float l = (float) m.toX(best->start), r = (float) m.toX(best->start + best->length);
    const float edge = std::min(6.0f, (r - l) / 3.0f);

    if (ly < 10.0f)
    {
        const float dIn = std::abs(p.x - (float) m.toX(best->start + best->fadeIn));
        const float dOut = std::abs(p.x - (float) m.toX(best->start + best->length - best->fadeOut));
        if (std::min(dIn, dOut) <= 12.0f)
        {
            hit.zone = dIn <= dOut ? HitZone::FadeIn : HitZone::FadeOut;
            return hit;
        }
    }
    if (p.x - l <= edge)
        hit.zone = HitZone::TrimStart;
    else if (r - p.x <= edge)
        hit.zone = HitZone::TrimEnd;
    else
        hit.zone = HitZone::Body;
    return hit;
}
} // namespace aligna
