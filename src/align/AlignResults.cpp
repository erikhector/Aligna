#include "align/AlignResults.h"
#include "edit/ClipOps.h"
#include <cmath>

namespace aligna
{
bool shouldPreTick(const AlignRow& r)
{
    return r.error.isEmpty() && r.result && r.result->confidence >= kConfidentThreshold;
}

SamplePos offsetSamples(const AlignRow& r) { return r.result ? r.result->newStart - r.oldStart : 0; }

juce::String formatOffsetMs(SamplePos samples, double rate)
{
    const double ms = rate > 0 ? 1000.0 * (double) samples / rate : 0.0;
    return (samples >= 0 ? "+" : "-") + juce::String(std::abs(ms), 3) + " ms";
}

juce::String formatOffsetSamples(SamplePos samples)
{
    return (samples >= 0 ? "+" : "-") + juce::String(samples < 0 ? -samples : samples) + " smp";
}

juce::String formatConfidence(float c) { return juce::String(c, 1); }

juce::String describeRow(const AlignRow& r, double rate)
{
    if (r.error.isNotEmpty() || ! r.result)
        return r.label + ": " + (r.error.isNotEmpty() ? r.error : juce::String("no result"));
    const auto off = offsetSamples(r);
    auto s = r.label + ": " + formatOffsetMs(off, rate) + " (" + formatOffsetSamples(off)
             + "), confidence " + formatConfidence(r.result->confidence);
    if (r.result->polarityInverted)
        s += ", polarity inverted";
    if (r.result->confidence < kConfidentThreshold)
        s += ", low confidence";
    return s;
}

bool selectionIsCapped(const juce::Range<SamplePos>& sel, double rate)
{
    return (double) sel.getLength() > kMaxRegionSeconds * rate;
}

bool applyAlignments(Project& p, const std::vector<std::pair<juce::String, SamplePos>>& moves)
{
    bool any = false;
    for (auto& [id, start] : moves)
    {
        int ti = -1;
        auto* c = findClip(p, id, &ti);
        if (c == nullptr)
            continue;
        const auto before = c->start;
        if (moveClip(p, id, start, ti) && findClip(p, id)->start != before)
            any = true;
    }
    return any;
}
} // namespace aligna
