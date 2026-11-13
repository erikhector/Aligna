#include "engine/SourcePool.h"
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <new>

namespace aligna
{
namespace
{
constexpr double maxSeconds = 2.0 * 60.0 * 60.0;

// Resamples one channel so that output sample 0 corresponds to input time 0.
std::vector<float> resampleChannel(const float* in, juce::int64 inLen, juce::int64 outLen, double speedRatio)
{
    const int latency = (int) juce::WindowedSincInterpolator::getBaseLatency();
    const juce::int64 needed = (juce::int64) std::ceil((double) outLen * speedRatio) + 4 * latency + 16;
    std::vector<float> padded((size_t) std::max<juce::int64>(needed, inLen + latency), 0.f);
    std::copy(in, in + inLen, padded.begin());

    juce::WindowedSincInterpolator interp;
    std::vector<float> out((size_t) outLen);
    std::vector<float> scratch((size_t) latency);

    // Prime: run `latency` samples through at ratio 1 and discard, which compensates the filter delay.
    juce::int64 inPos = (juce::int64) interp.process(1.0, padded.data(), scratch.data(), latency);

    juce::int64 outPos = 0;
    constexpr juce::int64 chunk = 16384;
    while (outPos < outLen)
    {
        auto n = (int) std::min(chunk, outLen - outPos);
        inPos += interp.process(speedRatio, padded.data() + inPos, out.data() + outPos, n);
        outPos += n;
    }
    return out;
}
} // namespace

SourcePool::SourcePool() { formats.registerBasicFormats(); }

AudioData SourcePool::decode(const juce::File& file, double projectRate, juce::String& error)
{
    try
    {
        return decodeImpl(file, projectRate, error);
    }
    catch (const std::bad_alloc&)
    {
        error = "Not enough memory to load this file";
        return nullptr;
    }
}

AudioData SourcePool::decodeImpl(const juce::File& file, double projectRate, juce::String& error)
{
    if (! file.existsAsFile())
    {
        error = "Could not open file";
        return nullptr;
    }
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr)
    {
        error = file.createInputStream() == nullptr ? "Could not open file" : "Unsupported audio format";
        return nullptr;
    }
    if (reader->lengthInSamples <= 0 || reader->numChannels == 0 || reader->sampleRate <= 0)
    {
        error = "File contains no audio";
        return nullptr;
    }

    const double fileRate = reader->sampleRate;
    const bool resample = std::abs(fileRate - projectRate) > 1e-6;
    const auto outLen = resample ? (juce::int64) std::llround((double) reader->lengthInSamples * projectRate / fileRate)
                                 : (juce::int64) reader->lengthInSamples;
    if ((double) outLen > maxSeconds * projectRate)
    {
        error = "File is too long (max 2 hours)";
        return nullptr;
    }
    if (outLen <= 0)
    {
        error = "File contains no audio";
        return nullptr;
    }

    const int srcCh = (int) reader->numChannels;
    juce::AudioBuffer<float> raw(srcCh, (int) reader->lengthInSamples);
    if (! reader->read(&raw, 0, (int) reader->lengthInSamples, 0, true, true))
    {
        error = "Could not open file";
        return nullptr;
    }

    // Channel layout: mono stays mono, stereo stays stereo, more is mixed down (even -> L, odd -> R).
    const int outCh = srcCh == 1 ? 1 : 2;
    juce::AudioBuffer<float> layout(outCh, raw.getNumSamples());
    if (srcCh <= 2)
    {
        for (int c = 0; c < outCh; ++c)
            layout.copyFrom(c, 0, raw, c, 0, raw.getNumSamples());
    }
    else
    {
        layout.clear();
        const int nL = (srcCh + 1) / 2, nR = srcCh / 2;
        for (int c = 0; c < srcCh; ++c)
            layout.addFrom(c % 2, 0, raw, c, 0, raw.getNumSamples(), 1.0f / (float) (c % 2 == 0 ? nL : nR));
    }
    raw.setSize(0, 0);

    if (! resample)
        return std::make_shared<const juce::AudioBuffer<float>>(std::move(layout));

    auto result = std::make_shared<juce::AudioBuffer<float>>(outCh, (int) outLen);
    for (int c = 0; c < outCh; ++c)
    {
        auto o = resampleChannel(layout.getReadPointer(c), layout.getNumSamples(), outLen, fileRate / projectRate);
        std::copy(o.begin(), o.end(), result->getWritePointer(c));
    }
    return result;
}

std::optional<SourceInfo> SourcePool::import(const juce::File& file, double projectRate, juce::String& error)
{
    auto data = decode(file, projectRate, error);
    if (data == nullptr)
        return std::nullopt;
    SourceInfo info;
    info.id = newId();
    info.path = file.getFullPathName();
    info.length = data->getNumSamples();
    info.numChannels = data->getNumChannels();
    info.missing = false;
    buffers[info.id] = std::move(data);
    return info;
}

bool SourcePool::load(SourceInfo& info, double projectRate)
{
    juce::String error;
    auto data = decode(juce::File(info.path), projectRate, error);
    if (data == nullptr)
    {
        info.missing = true;
        buffers.erase(info.id);
        return false;
    }
    info.length = data->getNumSamples();
    info.numChannels = data->getNumChannels();
    info.missing = false;
    buffers[info.id] = std::move(data);
    return true;
}

AudioData SourcePool::get(const juce::String& id) const
{
    auto it = buffers.find(id);
    return it == buffers.end() ? nullptr : it->second;
}

void SourcePool::clear() { buffers.clear(); }
} // namespace aligna
