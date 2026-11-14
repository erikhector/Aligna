#include "edit/RecordTiming.h"
#include "model/Grid.h"
#include <algorithm>
#include <cmath>

namespace aligna
{
double clampLatencyOffsetMs(double ms)
{
    if (!std::isfinite(ms)) // std::clamp passes NaN through
        return 0.0;
    return std::clamp(ms, kMinLatencyOffsetMs, kMaxLatencyOffsetMs);
}

SamplePos prerollStart(const Project& p, SamplePos recordStart, bool prerollOn)
{
    if (!prerollOn)
        return recordStart;
    auto bar = (SamplePos) std::llround(samplesPerBeat(p) * p.beatsPerBar);
    return std::max<SamplePos>(0, recordStart - bar);
}

SamplePos totalLatency(const LatencyInputs& in)
{
    double deviceSamples = (double) in.inputLatency + (double) in.outputLatency + in.chainDelay;
    double scale = in.deviceRate > 0 ? in.projectRate / in.deviceRate : 1.0;
    return (SamplePos) std::llround(deviceSamples * scale + in.manualOffsetMs * 0.001 * in.projectRate);
}

std::optional<TakePlacement> placeTake(SamplePos preroll, SamplePos recordStart, SamplePos latency,
                                       SamplePos recordedLength, double projectRate)
{
    TakePlacement t;
    t.start = recordStart;
    t.sourceOffset = (recordStart - preroll) + latency;
    if (t.sourceOffset < 0)
    {
        t.start = recordStart - t.sourceOffset;
        t.sourceOffset = 0;
    }
    t.length = recordedLength - t.sourceOffset;
    if (t.length < (SamplePos) std::llround(0.1 * projectRate))
        return std::nullopt;
    return t;
}

juce::String nextTakeName(const Project& p)
{
    // Numbers of more than 9 digits are not counted, so highest + 1 always fits in an int.
    constexpr int kMaxTakeDigits = 9;
    int highest = 0;
    for (auto& t : p.tracks)
    {
        auto digits = t.name.substring(5);
        if (t.name.startsWith("Take ") && digits.isNotEmpty() && digits.length() <= kMaxTakeDigits
            && digits.containsOnly("0123456789"))
            highest = std::max(highest, digits.getIntValue());
    }
    return "Take " + juce::String(highest + 1);
}
} // namespace aligna
