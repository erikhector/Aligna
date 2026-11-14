#pragma once
#include "model/Project.h"
#include <atomic>
#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>
#include <vector>

namespace aligna
{
// Streams audio-thread input to a 24-bit WAV via a ThreadedWriter.
// open()/close() are message-thread; begin/process/deviceStopped are real-time safe.
class Recorder
{
public:
    explicit Recorder(int bufferSamples = 0); // 0 = 10 s at the device rate given to open()
    ~Recorder();

    bool open(const juce::File& file, double deviceRate, int numChannels, juce::String& error);
    // As above, but writes to `stream` instead of creating `file` (which close() still reports). For tests.
    bool open(std::unique_ptr<juce::OutputStream> stream, const juce::File& file, double deviceRate, int numChannels,
              juce::String& error);
    void begin() noexcept;
    void process(const float* const* in, int numIn, int numSamples) noexcept;
    void deviceStopped() noexcept;

    struct Result
    {
        juce::File file;
        SamplePos numSamples = 0;
        double deviceRate = 0;
    };
    Result close();

    bool isOpen() const { return opened.load(); }
    bool isCapturing() const { return capturing.load(); }
    // Set from the audio thread (overflow, device) or the writer thread (a failed disk write); capture stops.
    bool hasFailed() const { return failure.load() != 0; }
    juce::String failureMessage() const;
    SamplePos samplesCaptured() const { return written.load(); } // per channel, since begin()
    float getAndResetPeak() noexcept;

private:
    static constexpr int zeroLength = 8192;
    enum Failure { none = 0, overflow = 1, device = 2, disk = 3 };
    class CheckedWriter;

    int requestedBuffer;
    juce::TimeSliceThread thread;
    juce::SpinLock lock; // guards `writer`; audio thread only try-locks
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
    juce::File file;
    double rate = 0;
    int channels = 0;
    std::vector<float> zeros;
    std::vector<const float*> pointers; // scratch, sized in open()

    std::atomic<bool> opened{false}, capturing{false};
    std::atomic<int> failure{none};
    std::atomic<float> peak{0.0f};
    std::atomic<SamplePos> written{0};
};
} // namespace aligna
