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
    using RB = RubberBand::RubberBandStretcher;
    const int channels = in.getNumChannels();
    const int inLen = in.getNumSamples();
    if (channels == 0 || inLen == 0)
        return juce::AudioBuffer<float>(channels, 0);

    // Compute in double (not int): a long source at a high ratio does not fit an int. Also rejects a NaN ratio.
    const double exactLen = std::round((double) inLen * ratio);
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

    std::vector<const float*> inPtrs((size_t) channels);
    std::vector<float*> outPtrs((size_t) channels);

    // Study pass (first half of progress).
    for (int pos = 0; pos < inLen; pos += kBlock)
    {
        const int n = std::min(kBlock, inLen - pos);
        for (int c = 0; c < channels; ++c)
            inPtrs[(size_t) c] = in.getReadPointer(c) + pos;
        rb.study(inPtrs.data(), (size_t) n, pos + n >= inLen);
        if (!report(0.5f * float(pos + n) / (float) inLen))
            return std::nullopt;
    }

    // Process pass. Offline mode compensates its own start delay, so output sample 0 is input time 0.
    int written = 0;
    const auto drain = [&]
    {
        int avail;
        while ((avail = rb.available()) > 0)
        {
            const int n = std::min(avail, std::max(0, outLen - written));
            if (n == 0)
            {
                // Over-long output: discard the surplus.
                std::vector<float> scratch((size_t) std::min(avail, kBlock));
                for (int c = 0; c < channels; ++c)
                    outPtrs[(size_t) c] = scratch.data();
                rb.retrieve(outPtrs.data(), scratch.size());
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

    for (int pos = 0; pos < inLen; pos += kBlock)
    {
        const int n = std::min(kBlock, inLen - pos);
        for (int c = 0; c < channels; ++c)
            inPtrs[(size_t) c] = in.getReadPointer(c) + pos;
        rb.process(inPtrs.data(), (size_t) n, pos + n >= inLen);
        drain();
        if (!report(0.5f + 0.5f * float(pos + n) / (float) inLen))
            return std::nullopt;
    }
    drain();
    // A short output stays zero-padded (out was cleared); an over-long one was trimmed above.
    return out;
}
} // namespace aligna
