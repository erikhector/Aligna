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
    const auto cb = clipBufferFor(s, clip);
    if (cb.buffer == nullptr)
        return out;
    const auto& buf = *cb.buffer;
    const int channels = buf.getNumChannels();
    if (channels <= 0)
        return out;

    // Timeline range covered by both the request and the clip, further limited by the buffer's extent (a stretched
    // buffer may hold only part of the source: cb.start is the stretched-source index of its first sample).
    const SamplePos origin = clip.start - clip.sourceOffset + cb.start; // timeline pos of buffer index 0
    SamplePos b = std::max(from, clip.start);
    SamplePos e = std::min(from + n, clip.start + clip.length);
    b = std::max(b, origin);
    e = std::min(e, origin + (SamplePos) buf.getNumSamples());
    const float scale = 1.0f / (float) channels;
    for (SamplePos pos = b; pos < e; ++pos)
    {
        const auto src = (int) (pos - origin);
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
    for (const Clip* c : { ref, tgt })
    {
        const auto* src = findSource(s.project, c->sourceId);
        // A missing or unknown source is never rendered: it takes the same silent path as an unstretched one.
        if (src == nullptr || src->missing)
            continue;
        // No buffer yet, or one that does not hold all of the clip yet (a larger render is on its way, or failed):
        // comparing against the silent part would give a wrong answer with confidence.
        const auto key = bufferKey(*c);
        if (! nearlyEqual(c->stretch, 1.0)
            && (s.audio.count(key) == 0 || s.partialStretch.count(key) != 0 || s.failedStretch.count(key) != 0))
        {
            error = s.failedStretch.count(key) != 0
                        ? juce::String("Clip could not be stretched, so it has no audio to compare")
                        : juce::String::fromUTF8("Clip is still stretching \xe2\x80\x94 try again in a moment");
            return std::nullopt;
        }
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

    // Not clamped to 0: a clamped start would be a wrong alignment reported with full confidence. A start before
    // the project start is kept negative and applied by trimming the clip's head (see ClipAlignment).
    const SamplePos newStart = tgt->start - offset->lag;
    if (newStart + tgt->length <= 0)
    {
        error = "Aligned clip would lie entirely before the beginning of the project";
        return std::nullopt;
    }
    return ClipAlignment { newStart, offset->confidence, offset->polarityInverted };
}
} // namespace aligna
