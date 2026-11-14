#include "edit/RecordTiming.h"
#include "model/Grid.h"

namespace aligna
{
double clampLatencyOffsetMs(double ms) { return std::clamp(ms, kMinLatencyOffsetMs, kMaxLatencyOffsetMs); }

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
    int highest = 0;
    for (auto& t : p.tracks)
        if (t.name.startsWith("Take ") && t.name.substring(5).containsOnly("0123456789") && t.name.length() > 5)
            highest = std::max(highest, t.name.substring(5).getIntValue());
    return "Take " + juce::String(highest + 1);
}
} // namespace aligna
