#include "align/AlignResults.h"
#include "edit/ClipOps.h"
#include <cmath>

namespace aligna
{
ClipAnchor captureAnchor(const Project& p, const juce::String& clipId)
{
    ClipAnchor a;
    a.clipId = clipId;
    for (size_t ti = 0; ti < p.tracks.size(); ++ti)
        for (auto& c : p.tracks[ti].clips)
            if (c.id == clipId)
            {
                a.exists = true;
                a.trackIndex = (int) ti;
                a.start = c.start;
                a.sourceOffset = c.sourceOffset;
                a.length = c.length;
                return a;
            }
    return a;
}

namespace
{
bool anchorMatches(const Project& p, const ClipAnchor& a)
{
    if (! a.exists)
        return false;
    const auto now = captureAnchor(p, a.clipId);
    return now.exists && now.trackIndex == a.trackIndex && now.start == a.start && now.sourceOffset == a.sourceOffset
           && now.length == a.length;
}
} // namespace

bool isStale(const Project& p, const ClipAnchor& reference, const AlignRow& row)
{
    return ! anchorMatches(p, row.anchor) || ! anchorMatches(p, reference);
}

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
    if (r.result->newStart < 0)
        s += ", start trimmed by " + formatOffsetMs(-r.result->newStart, rate).substring(1) + " to fit the project";
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
        const auto beforeStart = c->start;
        const auto beforeOffset = c->sourceOffset;
        bool ok = false;
        if (start < 0)
        {
            // Aligned position before the project start: drop the overshoot from the clip's head instead of
            // shifting its audio late. sourceOffset/length are timeline samples whether or not the clip is stretched.
            const SamplePos cut = -start;
            if (cut >= c->length)
                continue;
            ok = trimClipStart(p, id, c->start + cut) && moveClip(p, id, 0, ti);
        }
        else
        {
            ok = moveClip(p, id, start, ti);
        }
        c = findClip(p, id);
        if (ok && c != nullptr && (c->start != beforeStart || c->sourceOffset != beforeOffset))
            any = true;
    }
    return any;
}
} // namespace aligna
