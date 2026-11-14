#include "engine/Stretcher.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <rubberband/RubberBandStretcher.h>
#include <vector>

namespace aligna
{
namespace
{
constexpr int kBlock = 4096;
}

std::optional<juce::AudioBuffer<float>> stretchBuffer(const juce::AudioBuffer<float>& in, double sampleRate,
                                                      double ratio, const std::function<bool(float)>& progress)
{
    return stretchRange(in, 0, in.getNumSamples(), sampleRate, ratio, progress);
}

std::optional<juce::AudioBuffer<float>> stretchRange(const juce::AudioBuffer<float>& in, std::int64_t srcBegin,
                                                     std::int64_t srcEnd, double sampleRate, double ratio,
                                                     const std::function<bool(float)>& progress)
{
    using RB = RubberBand::RubberBandStretcher;
    const int channels = in.getNumChannels();
    const int begin = (int) std::clamp<std::int64_t>(srcBegin, 0, in.getNumSamples());
    const int inLen = (int) std::clamp<std::int64_t>(srcEnd, begin, in.getNumSamples()) - begin;
    if (channels == 0 || inLen == 0)
        return juce::AudioBuffer<float>(channels, 0);

    // Compute in double (not int): a long source at a high ratio does not fit an int. Also rejects a NaN ratio.
    // Both ends are rounded on the whole-source grid, so adjoining ranges neither overlap nor leave a gap.
    const double exactLen = std::round((double) (begin + inLen) * ratio) - std::round((double) begin * ratio);
    if (! (exactLen >= 1.0 && exactLen <= (double) std::numeric_limits<int>::max()))
        return std::nullopt;
    const int outLen = (int) exactLen;

    juce::AudioBuffer<float> out(channels, outLen);
    out.clear();

    const auto report = [&](float p) { return !progress || progress(juce::jlimit(0.0f, 1.0f, p)); };

    RB rb((size_t) sampleRate, (size_t) channels,
          RB::OptionProcessOffline | RB::OptionEngineFaster | RB::OptionTransientsMixed | RB::OptionDetectorCompound |
              RB::OptionThreadingNever | RB::OptionChannelsTogether);
    rb.setTimeRatio(ratio);
    rb.setPitchScale(1.0);
    rb.setExpectedInputDuration((size_t) inLen);

    // Rubber Band analyses a frame every `hop` input samples, counted from the first sample it is fed, and places
    // its output frame by frame. Where those frames fall decides how a transient comes out, to within tens of
    // samples. So that a range render has its frames where a whole render has them, feeding starts on the whole
    // render's hop grid: up to hop - 1 samples before `begin` (real audio), whose output is then discarded. With
    // that, transients land within a sample or two of where the whole render puts them.
    const int hop = std::max(1, (int) rb.getInputIncrement());
    const int feed = begin - begin % hop; // first source sample fed; `begin` itself for a whole render (begin == 0)
    const int feedLen = begin + inLen - feed;
    int skip = (int) (std::round((double) begin * ratio) - std::round((double) feed * ratio)); // output of the lead-in
    rb.setExpectedInputDuration((size_t) feedLen);

    std::vector<const float*> inPtrs((size_t) channels);
    std::vector<float*> outPtrs((size_t) channels);
    std::vector<float> scratch((size_t) kBlock); // for output that is thrown away

    // Study pass (first half of progress).
    for (int pos = 0; pos < feedLen; pos += kBlock)
    {
        const int n = std::min(kBlock, feedLen - pos);
        for (int c = 0; c < channels; ++c)
            inPtrs[(size_t) c] = in.getReadPointer(c) + feed + pos;
        rb.study(inPtrs.data(), (size_t) n, pos + n >= feedLen);
        if (!report(0.5f * float(pos + n) / (float) feedLen))
            return std::nullopt;
    }

    // Process pass. Offline mode compensates its own start delay, so output sample 0 is the first sample fed.
    int written = 0;
    const auto drain = [&]
    {
        int avail;
        while ((avail = rb.available()) > 0)
        {
            const int n = skip > 0 ? 0 : std::min(avail, std::max(0, outLen - written));
            if (n == 0)
            {
                // The lead-in's output, or over-long output: discard it.
                const int drop = std::min({ avail, kBlock, skip > 0 ? skip : kBlock });
                for (int c = 0; c < channels; ++c)
                    outPtrs[(size_t) c] = scratch.data();
                const auto dropped = (int) rb.retrieve(outPtrs.data(), (size_t) drop);
                skip = std::max(0, skip - dropped);
                if (dropped == 0)
                    break;
                continue;
            }
            for (int c = 0; c < channels; ++c)
                outPtrs[(size_t) c] = out.getWritePointer(c) + written;
            const auto got = (int) rb.retrieve(outPtrs.data(), (size_t) n);
            written += got;
            if (got == 0)
                break;
        }
    };

    for (int pos = 0; pos < feedLen; pos += kBlock)
    {
        const int n = std::min(kBlock, feedLen - pos);
        for (int c = 0; c < channels; ++c)
            inPtrs[(size_t) c] = in.getReadPointer(c) + feed + pos;
        rb.process(inPtrs.data(), (size_t) n, pos + n >= feedLen);
        drain();
        if (!report(0.5f + 0.5f * float(pos + n) / (float) feedLen))
            return std::nullopt;
    }
    drain();
    // A short output stays zero-padded (out was cleared); an over-long one was trimmed above.
    return out;
}
} // namespace aligna
