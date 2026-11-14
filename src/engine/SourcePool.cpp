#include "engine/SourcePool.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace aligna
{
namespace
{
constexpr double maxSeconds = 2.0 * 60.0 * 60.0;

// Frames read, and output samples resampled, between two progress calls: a cancel is noticed within one chunk.
constexpr int readChunk = 1 << 17;
constexpr juce::int64 resampleChunk = 16384;

// Resamples one channel into `out` so that output sample 0 corresponds to input time 0.
// onChunk(fraction of this channel) is called after every chunk; it may throw to abandon the decode.
template <typename OnChunk>
void resampleChannel(const float* in, juce::int64 inLen, float* out, juce::int64 outLen, double speedRatio,
                     OnChunk&& onChunk)
{
    const int latency = (int) juce::WindowedSincInterpolator::getBaseLatency();
    const juce::int64 needed = (juce::int64) std::ceil((double) outLen * speedRatio) + 4 * latency + 16;
    std::vector<float> padded((size_t) std::max<juce::int64>(needed, inLen + latency), 0.f);
    std::copy(in, in + inLen, padded.begin());

    juce::WindowedSincInterpolator interp;
    std::vector<float> scratch((size_t) latency);

    // Prime: run `latency` samples through at ratio 1 and discard, which compensates the filter delay.
    juce::int64 inPos = (juce::int64) interp.process(1.0, padded.data(), scratch.data(), latency);

    juce::int64 outPos = 0;
    while (outPos < outLen)
    {
        auto n = (int) std::min(resampleChunk, outLen - outPos);
        inPos += interp.process(speedRatio, padded.data() + inPos, out + outPos, n);
        outPos += n;
        onChunk((double) outPos / (double) outLen);
    }
}

// True for the readers whose payload is plain PCM/float frames (JUCE's WAV and AIFF readers accept nothing else;
// Ogg-in-WAV is handed to the Ogg reader and reports that format's name).
bool isUncompressedFormat(const juce::AudioFormatReader& r)
{
    return r.getFormatName() == "WAV file" || r.getFormatName() == "AIFF file";
}

// Replaces NaN/Inf with 0. Magnitude is left alone: float files may legitimately exceed 1.0.
void zeroNonFinite(float* d, int n)
{
    for (int i = 0; i < n; ++i)
        if (! std::isfinite(d[i]))
            d[i] = 0.0f;
}

struct DecodeCancelled
{
};

AudioData decodeImpl(const juce::File& file, double projectRate, juce::String& error, const DecodeProgress& progress)
{
    // Throws on cancel, so every buffer of the half-done decode is released on the way out.
    auto report = [&progress](double fraction) {
        if (progress && ! progress((float) juce::jlimit(0.0, 1.0, fraction)))
            throw DecodeCancelled();
    };
    report(0.0);
    if (! file.existsAsFile())
    {
        error = "Could not open file";
        return nullptr;
    }
    // A manager of its own: nothing is shared with the message thread or with another decode.
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
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

    // The WAV/AIFF readers trust the header's data length. Their payload is uncompressed, so it can never hold more
    // frames than the file has bytes for: a header that claims more (a cut-off download, or a hostile file) is read
    // up to what is really there, instead of allocating and zero-filling memory for the claimed length.
    auto frames = (juce::int64) reader->lengthInSamples;
    if (isUncompressedFormat(*reader))
    {
        const auto bytesPerFrame = (juce::int64) reader->numChannels * (juce::int64) (reader->bitsPerSample / 8);
        if (bytesPerFrame > 0)
            frames = std::min(frames, file.getSize() / bytesPerFrame);
        if (frames <= 0)
        {
            error = "File contains no audio";
            return nullptr;
        }
    }
    // Bound the file's own length before any cast or allocation (e.g. hours of 352.8 kHz audio exceed int).
    if (frames > (juce::int64) std::numeric_limits<int>::max())
    {
        error = "File is too long to load";
        return nullptr;
    }
    const int fileLen = (int) frames;

    const double fileRate = reader->sampleRate;
    const bool resample = std::abs(fileRate - projectRate) > 1e-6;
    const auto outLen = resample ? (juce::int64) std::llround((double) fileLen * projectRate / fileRate)
                                 : (juce::int64) fileLen;
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

    // Share of the progress taken by reading the file; the rest is the resampler, one equal part per channel.
    const double readShare = resample ? 0.25 : 1.0;
    const int srcCh = (int) reader->numChannels;
    juce::AudioBuffer<float> raw(srcCh, fileLen);
    for (int pos = 0; pos < fileLen;)
    {
        const int n = std::min(readChunk, fileLen - pos);
        if (! reader->read(&raw, pos, n, pos, true, true))
        {
            error = "Could not open file";
            return nullptr;
        }
        pos += n;
        report(readShare * (double) pos / (double) fileLen);
    }
    reader.reset();
    // Float files can carry NaN/Inf, which nothing downstream (device, export, alignment, LAME) expects.
    for (int c = 0; c < srcCh; ++c)
        zeroNonFinite(raw.getWritePointer(c), fileLen);

    // Channel layout: mono stays mono, stereo stays stereo, more is mixed down (even -> L, odd -> R).
    const int outCh = srcCh == 1 ? 1 : 2;
    juce::AudioBuffer<float> layout;
    if (srcCh <= 2)
    {
        layout = std::move(raw); // already the right layout: no second copy
    }
    else
    {
        layout.setSize(outCh, fileLen);
        layout.clear();
        const int nL = (srcCh + 1) / 2, nR = srcCh / 2;
        for (int c = 0; c < srcCh; ++c)
            layout.addFrom(c % 2, 0, raw, c, 0, fileLen, 1.0f / (float) (c % 2 == 0 ? nL : nR));
        raw.setSize(0, 0);
    }

    if (! resample)
        return std::make_shared<const juce::AudioBuffer<float>>(std::move(layout));

    auto result = std::make_shared<juce::AudioBuffer<float>>(outCh, (int) outLen);
    for (int c = 0; c < outCh; ++c)
        resampleChannel(layout.getReadPointer(c), layout.getNumSamples(), result->getWritePointer(c), outLen,
                        fileRate / projectRate, [&](double f) {
                            report(readShare + (1.0 - readShare) * ((double) c + f) / (double) outCh);
                        });
    return result;
}
} // namespace

DecodedAudio SourcePool::decodeFile(const juce::File& file, double projectRate, const DecodeProgress& progress)
{
    DecodedAudio d;
    try
    {
        d.audio = decodeImpl(file, projectRate, d.error, progress);
    }
    catch (const DecodeCancelled&)
    {
        d.cancelled = true;
    }
    catch (const std::bad_alloc&)
    {
        d.error = "Not enough memory to load this file";
    }
    return d;
}

bool DecodeBatch::run(const std::function<bool(int, float)>& progress)
{
    for (size_t i = 0; i < items.size(); ++i)
    {
        auto d = SourcePool::decodeFile(items[i].file, projectRate, [&progress, i](float f) {
            return ! progress || progress((int) i, f);
        });
        if (d.cancelled)
        {
            for (auto& it : items)
                it.audio = nullptr;
            return false;
        }
        items[i].audio = std::move(d.audio);
        items[i].error = std::move(d.error);
    }
    return true;
}

SourceInfo SourcePool::store(const juce::File& file, AudioData data)
{
    jassert(data != nullptr);
    SourceInfo info;
    info.id = newId();
    info.path = file.getFullPathName();
    info.length = data->getNumSamples();
    info.numChannels = data->getNumChannels();
    info.missing = false;
    buffers[info.id] = std::move(data);
    return info;
}

bool SourcePool::store(SourceInfo& info, AudioData data)
{
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

std::optional<SourceInfo> SourcePool::import(const juce::File& file, double projectRate, juce::String& error)
{
    auto d = decodeFile(file, projectRate);
    if (d.audio == nullptr)
    {
        error = d.error;
        return std::nullopt;
    }
    return store(file, std::move(d.audio));
}

bool SourcePool::load(SourceInfo& info, double projectRate)
{
    return store(info, decodeFile(juce::File(info.path), projectRate).audio);
}

AudioData SourcePool::get(const juce::String& id) const
{
    auto it = buffers.find(id);
    return it == buffers.end() ? nullptr : it->second;
}

void SourcePool::clear() { buffers.clear(); }

void SourcePool::remove(const juce::String& id) { buffers.erase(id); }

void SourcePool::retainOnly(const std::set<juce::String>& ids)
{
    std::erase_if(buffers, [&ids](const auto& b) { return ids.count(b.first) == 0; });
}
} // namespace aligna
