#include "engine/Mixer.h"
#include <cmath>

namespace aligna
{
std::shared_ptr<const MixSnapshot> makeSnapshot(const Project& p, const SourcePool& pool)
{
    auto snap = std::make_shared<MixSnapshot>();
    snap->project = p;
    for (auto& t : snap->project.tracks)
        std::stable_sort(t.clips.begin(), t.clips.end(),
                         [](const Clip& a, const Clip& b) { return a.start < b.start; });
    for (auto& s : snap->project.sources)
    {
        if (s.missing)
            continue;
        if (auto d = pool.get(s.id))
            snap->audio[s.id] = d;
    }
    return snap;
}

namespace
{
struct TrackCtx
{
    const MixSnapshot* snap;
    const Track* track;
    float trackGain, pan;
    SamplePos blockStart;
    juce::AudioBuffer<float>* out;
    int offset;
};

void renderClipRange(const TrackCtx& c, const Clip& clip, SamplePos a, SamplePos b)
{
    auto it = c.snap->audio.find(clip.sourceId);
    if (it == c.snap->audio.end() || it->second == nullptr)
        return;
    const auto& src = *it->second;
    const int srcCh = src.getNumChannels();
    if (srcCh < 1)
        return;
    const SamplePos srcLen = src.getNumSamples();

    // R6: clamp the read range to the decoded buffer.
    SamplePos lo = juce::jmax(a, clip.start);
    SamplePos hi = juce::jmin(b, clip.start + clip.length);
    // source index = sourceOffset + (t - clip.start) must lie in [0, srcLen)
    lo = juce::jmax(lo, clip.start - clip.sourceOffset);
    hi = juce::jmin(hi, clip.start - clip.sourceOffset + srcLen);
    if (lo >= hi)
        return;

    const float clipGain = juce::Decibels::decibelsToGain(clip.gainDb);
    const float g = clipGain * c.trackGain;
    const bool mono = srcCh == 1;
    float gl, gr;
    if (mono)
    {
        const auto ang = (static_cast<double>(c.pan) + 1.0) * juce::MathConstants<double>::pi / 4.0;
        gl = static_cast<float>(std::cos(ang));
        gr = static_cast<float>(std::sin(ang));
    }
    else
    {
        gl = juce::jmin(1.0f, 1.0f - c.pan);
        gr = juce::jmin(1.0f, 1.0f + c.pan);
    }
    gl = juce::jmax(0.0f, gl) * g;
    gr = juce::jmax(0.0f, gr) * g;

    const float* sl = src.getReadPointer(0);
    const float* sr = src.getReadPointer(mono ? 0 : 1);
    float* ol = c.out->getWritePointer(0);
    float* orr = c.out->getWritePointer(1);

    for (SamplePos t = lo; t < hi; ++t)
    {
        const SamplePos i = t - clip.start; // clip-local
        double fade = 1.0;
        if (clip.fadeIn > 0 && i < clip.fadeIn)
            fade = juce::jlimit(0.0, 1.0, static_cast<double>(i) / static_cast<double>(clip.fadeIn));
        if (clip.fadeOut > 0)
        {
            const SamplePos fromEnd = clip.length - 1 - i;
            if (fromEnd < clip.fadeOut)
                fade = juce::jmin(fade, juce::jlimit(0.0, 1.0, static_cast<double>(fromEnd) / static_cast<double>(clip.fadeOut)));
        }
        const auto si = static_cast<int>(clip.sourceOffset + i);
        const auto oi = c.offset + static_cast<int>(t - c.blockStart);
        const auto f = static_cast<float>(fade);
        ol[oi] += sl[si] * gl * f;
        orr[oi] += sr[si] * gr * f;
    }
}

// Renders [lo,hi) using clips [0..idx]; later clips (higher index) cover earlier ones.
void renderRange(const TrackCtx& c, int idx, SamplePos lo, SamplePos hi)
{
    for (; idx >= 0 && lo < hi; --idx)
    {
        const auto& clip = c.track->clips[static_cast<size_t>(idx)];
        const SamplePos a = juce::jmax(lo, clip.start);
        const SamplePos b = juce::jmin(hi, clip.start + clip.length);
        if (a >= b)
            continue;
        renderClipRange(c, clip, a, b);
        if (a > lo)
            renderRange(c, idx - 1, lo, a);
        lo = b; // right remainder handled by the loop
    }
}
} // namespace

void renderMix(const MixSnapshot& snap, SamplePos start, juce::AudioBuffer<float>& out, int offset, int numSamples)
{
    jassert(out.getNumChannels() >= 2);
    jassert(offset >= 0 && numSamples >= 0 && offset + numSamples <= out.getNumSamples());
    if (out.getNumChannels() < 2 || numSamples <= 0 || offset < 0 || offset + numSamples > out.getNumSamples())
        return;

    bool anySolo = false;
    for (auto& t : snap.project.tracks)
        anySolo = anySolo || t.solo;

    for (auto& t : snap.project.tracks)
    {
        if (t.mute || (anySolo && !t.solo))
            continue;
        TrackCtx c{ &snap, &t, juce::Decibels::decibelsToGain(t.volumeDb), juce::jlimit(-1.0f, 1.0f, t.pan),
                    start, &out, offset };
        renderRange(c, static_cast<int>(t.clips.size()) - 1, start, start + numSamples);
    }
}

bool exportMix(const MixSnapshot& snap, const juce::File& wav, int bitDepth, juce::String& error,
               const std::function<bool(float)>& progress)
{
    if (bitDepth != 16 && bitDepth != 24)
    {
        error = "Unsupported bit depth";
        return false;
    }
    const auto total = projectEnd(snap.project);
    if (total <= 0)
    {
        error = "Nothing to export";
        return false;
    }

    const auto tmp = wav.getSiblingFile(wav.getFileName() + ".tmp");
    tmp.deleteFile();
    auto stream = std::make_unique<juce::FileOutputStream>(tmp);
    if (stream->failedToOpen())
    {
        error = "Could not write " + wav.getFullPathName();
        return false;
    }

    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        fmt.createWriterFor(stream.get(), snap.project.sampleRate, 2, bitDepth, {}, 0));
    if (writer == nullptr)
    {
        stream.reset();
        tmp.deleteFile();
        error = "Could not create WAV writer";
        return false;
    }
    stream.release(); // owned by writer

    constexpr int blockSize = 4096;
    juce::AudioBuffer<float> buf(2, blockSize);
    bool ok = true;
    for (SamplePos pos = 0; pos < total; pos += blockSize)
    {
        const int n = static_cast<int>(juce::jmin<SamplePos>(blockSize, total - pos));
        buf.clear();
        renderMix(snap, pos, buf, 0, n);
        if (! writer->writeFromAudioSampleBuffer(buf, 0, n))
        {
            error = "Write failed";
            ok = false;
            break;
        }
        if (progress && ! progress(static_cast<float>(static_cast<double>(pos + n) / static_cast<double>(total))))
        {
            error = "Export cancelled";
            ok = false;
            break;
        }
    }
    writer.reset(); // flush + close
    if (! ok)
    {
        tmp.deleteFile();
        return false;
    }
    if (! tmp.moveFileTo(wav))
    {
        tmp.deleteFile();
        error = "Could not write " + wav.getFullPathName();
        return false;
    }
    return true;
}
} // namespace aligna
