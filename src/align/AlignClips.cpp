#include "align/AlignClips.h"
#include <algorithm>
#include <cmath>

namespace aligna
{
namespace
{
const Clip* findClipConst(const Project& p, const juce::String& id)
{
    for (auto& t : p.tracks)
        for (auto& c : t.clips)
            if (c.id == id)
                return &c;
    return nullptr;
}

// Renders `clip` alone (mono = channel average, unity gain, no fades) over timeline [from, from + n).
// Samples outside the clip's extent or the decoded buffer are 0.
std::vector<float> renderClipAlone(const MixSnapshot& s, const Clip& clip, SamplePos from, SamplePos n)
{
    std::vector<float> out((size_t) std::max<SamplePos>(0, n), 0.0f);
    const auto it = s.audio.find(clip.sourceId);
    if (it == s.audio.end() || it->second == nullptr)
        return out;
    const auto& buf = *it->second;
    const int channels = buf.getNumChannels();
    if (channels <= 0)
        return out;

    // Timeline range covered by both the request and the clip, further limited by the buffer's extent.
    SamplePos b = std::max(from, clip.start);
    SamplePos e = std::min(from + n, clip.start + clip.length);
    b = std::max(b, clip.start - clip.sourceOffset);
    e = std::min(e, clip.start - clip.sourceOffset + (SamplePos) buf.getNumSamples());
    const float scale = 1.0f / (float) channels;
    for (SamplePos pos = b; pos < e; ++pos)
    {
        const auto src = (int) (clip.sourceOffset + (pos - clip.start));
        float v = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
            v += buf.getSample(ch, src);
        out[(size_t) (pos - from)] = v * scale;
    }
    return out;
}
} // namespace

std::optional<ClipAlignment> computeAlignment(const MixSnapshot& s, const juce::String& referenceClipId,
                                              const juce::String& targetClipId,
                                              std::optional<juce::Range<SamplePos>> selection, double maxShiftSeconds,
                                              const std::function<bool(float)>& progress, juce::String& error)
{
    const Clip* ref = findClipConst(s.project, referenceClipId);
    const Clip* tgt = findClipConst(s.project, targetClipId);
    if (ref == nullptr || tgt == nullptr)
    {
        error = "Clip not found";
        return std::nullopt;
    }
    if (referenceClipId == targetClipId)
    {
        error = "Choose two different clips";
        return std::nullopt;
    }

    const double rate = s.project.sampleRate;
    const auto maxLag =
        (SamplePos) std::llround(std::clamp(maxShiftSeconds, 0.0, kMaxShiftSecondsLimit) * rate);
    const auto maxRegion = (SamplePos) std::llround(kMaxRegionSeconds * rate);

    const juce::Range<SamplePos> refRange(ref->start, ref->start + ref->length);
    juce::Range<SamplePos> region;
    if (selection)
        region = selection->getIntersectionWith(refRange);
    else
        region = refRange.getIntersectionWith({ tgt->start - maxLag, tgt->start + tgt->length + maxLag });
    region = region.withLength(std::min(region.getLength(), maxRegion));

    if (region.isEmpty())
    {
        error = juce::String::fromUTF8("Clips are too far apart \xe2\x80\x94 increase the search range");
        return std::nullopt;
    }

    const auto refSignal = renderClipAlone(s, *ref, region.getStart(), region.getLength());
    const auto tgtSignal = renderClipAlone(s, *tgt, region.getStart() - maxLag, region.getLength() + 2 * maxLag);

    const auto offset = findOffset(refSignal, tgtSignal, -maxLag, maxLag, rate, progress, error);
    if (!offset)
        return std::nullopt;

    return ClipAlignment { std::max<SamplePos>(0, tgt->start - offset->lag), offset->confidence,
                           offset->polarityInverted };
}
} // namespace aligna
