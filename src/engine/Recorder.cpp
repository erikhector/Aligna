#include "engine/Recorder.h"
#include <algorithm>
#include <cmath>

namespace aligna
{
// ThreadedWriter drops the result of every write it makes on its background thread, so a full or removed disk
// would go unnoticed. This forwards to the real writer and records a failed write or flush in the Recorder.
class Recorder::CheckedWriter : public juce::AudioFormatWriter
{
public:
    CheckedWriter(std::unique_ptr<juce::AudioFormatWriter> w, Recorder& r)
        : juce::AudioFormatWriter(nullptr, w->getFormatName(), w->getSampleRate(), w->getNumChannels(),
                                  w->getBitsPerSample()),
          inner(std::move(w)), owner(r)
    {
        usesFloatingPointData = inner->isFloatingPoint();
    }

    bool write(const int** samples, int numSamples) override { return check(inner->write(samples, numSamples)); }
    bool flush() override { return check(inner->flush()); }

private:
    bool check(bool ok)
    {
        if (!ok)
        {
            int expected = none; // an earlier failure keeps its message
            owner.failure.compare_exchange_strong(expected, disk);
            owner.capturing = false;
        }
        return ok;
    }

    std::unique_ptr<juce::AudioFormatWriter> inner;
    Recorder& owner;
};

Recorder::Recorder(int bufferSamples) : requestedBuffer(bufferSamples), thread("Aligna recorder") {}

Recorder::~Recorder()
{
    if (isOpen())
        close();
    thread.stopThread(2000);
}

bool Recorder::open(const juce::File& f, double deviceRate, int numChannels, juce::String& error)
{
    if (isOpen())
        close();
    if (numChannels < 1 || deviceRate <= 0.0)
    {
        error = "Invalid recording format.";
        return false;
    }
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    auto stream = f.createOutputStream();
    if (stream == nullptr)
    {
        error = "Could not create " + f.getFullPathName();
        return false;
    }
    return open(std::move(stream), f, deviceRate, numChannels, error);
}

bool Recorder::open(std::unique_ptr<juce::OutputStream> stream, const juce::File& f, double deviceRate,
                    int numChannels, juce::String& error)
{
    if (isOpen())
        close();
    if (stream == nullptr || numChannels < 1 || deviceRate <= 0.0)
    {
        error = "Invalid recording format.";
        return false;
    }
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::AudioFormatWriter> w(
        fmt.createWriterFor(stream.get(), deviceRate, (unsigned) numChannels, 24, {}, 0));
    if (w == nullptr)
    {
        error = "Could not write a WAV file at " + juce::String(deviceRate) + " Hz.";
        return false;
    }
    stream.release(); // owned by the writer now

    zeros.assign((size_t) zeroLength, 0.0f);
    pointers.assign((size_t) numChannels, nullptr);
    file = f;
    rate = deviceRate;
    channels = numChannels;
    failure = none;
    peak = 0.0f;
    written = 0;
    capturing = false;

    if (! thread.isThreadRunning())
        thread.startThread();
    const int buffer = requestedBuffer > 0 ? requestedBuffer : (int) (deviceRate * 10.0);
    auto tw = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(new CheckedWriter(std::move(w), *this),
                                                                        thread, buffer);
    tw->setFlushInterval((int) deviceRate);
    {
        const juce::SpinLock::ScopedLockType sl(lock);
        writer = std::move(tw);
    }
    opened = true;
    return true;
}

void Recorder::begin() noexcept
{
    if (opened.load() && failure.load() == none)
        capturing = true;
}

void Recorder::process(const float* const* in, int numIn, int numSamples) noexcept
{
    float p = 0.0f;
    for (int c = 0; c < numIn; ++c)
        for (int i = 0; i < numSamples; ++i)
            p = std::max(p, std::abs(in[c][i]));
    float cur = peak.load(std::memory_order_relaxed);
    while (p > cur && ! peak.compare_exchange_weak(cur, p, std::memory_order_relaxed)) {}

    if (! capturing.load() || numSamples <= 0)
        return;
    const juce::SpinLock::ScopedTryLockType sl(lock);
    if (! sl.isLocked() || writer == nullptr)
        return;

    for (int pos = 0; pos < numSamples;)
    {
        const int n = std::min(numSamples - pos, zeroLength);
        for (int c = 0; c < channels; ++c)
            pointers[(size_t) c] = c < numIn ? in[c] + pos : zeros.data();
        if (! writer->write(pointers.data(), n))
        {
            failure = overflow;
            capturing = false;
            return;
        }
        written.fetch_add(n);
        pos += n;
    }
}

void Recorder::deviceStopped() noexcept
{
    failure = device;
    capturing = false;
}

Recorder::Result Recorder::close()
{
    Result r;
    capturing = false;
    opened = false;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> old;
    {
        const juce::SpinLock::ScopedLockType sl(lock);
        old = std::move(writer);
    }
    old.reset(); // drains pending data and finalises the WAV header
    r.file = file;
    r.numSamples = written.load();
    r.deviceRate = rate;
    return r;
}

juce::String Recorder::failureMessage() const
{
    switch (failure.load())
    {
        case overflow: return "Recording stopped: the disk could not keep up.";
        case device: return "Recording stopped: the audio device stopped or changed.";
        case disk: return "Recording stopped: writing to the disk failed. It may be full or disconnected.";
        default: return {};
    }
}

float Recorder::getAndResetPeak() noexcept { return peak.exchange(0.0f); }
} // namespace aligna
