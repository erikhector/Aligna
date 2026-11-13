#include "edit/StretchMath.h"
#include <algorithm>

namespace aligna
{
double stretchForEdge(const Clip& c, SamplePos newEnd)
{
    const auto newLength = std::max<SamplePos>(1, newEnd - c.start);
    return c.stretch * static_cast<double>(newLength) / static_cast<double>(std::max<SamplePos>(1, c.length));
}

double clipTempoRatio(const Project& p, const Clip& c, double sourceBpm, bool* limited)
{
    const double wanted = std::clamp(sourceBpm, 20.0, 400.0) / p.bpm;
    const double r = std::clamp(wanted, kMinStretch, std::max(kMinStretch, maxStretchFor(p, c)));
    if (limited != nullptr)
        *limited = !nearlyEqual(r, wanted);
    return r;
}

double TapTempo::tap(double seconds)
{
    constexpr double resetGap = 2.0;
    constexpr size_t maxTaps = 8;
    if (!taps.empty() && seconds - taps.back() >= resetGap)
        taps.clear();
    taps.push_back(seconds);
    if (taps.size() > maxTaps)
        taps.erase(taps.begin());
    if (taps.size() < 2)
        return 0.0;
    const double mean = (taps.back() - taps.front()) / static_cast<double>(taps.size() - 1);
    return mean > 0.0 ? 60.0 / mean : 0.0;
}
} // namespace aligna
