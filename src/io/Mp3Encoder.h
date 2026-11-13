#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <memory>
#include <vector>

namespace aligna
{
// Stereo CBR MP3 writer (bundled LAME). Not thread-safe; one export at a time.
class Mp3Encoder
{
public:
    Mp3Encoder();
    Mp3Encoder(const Mp3Encoder&) = delete;
    Mp3Encoder& operator=(const Mp3Encoder&) = delete;

    // Opens `file` for writing. outRate = inRate if in {32000,44100,48000} else 44100.
    // kbps must be 192, 256 or 320. Returns false and sets error otherwise.
    bool open(const juce::File& file, double inRate, int kbps, const juce::String& title, juce::String& error);
    bool write(const juce::AudioBuffer<float>& stereo, int numSamples, juce::String& error); // channels 0,1
    bool finish(juce::String& error); // flush + write LAME/Xing tag + close
    ~Mp3Encoder();                    // closes without finishing if needed

    static int outputRateFor(double inRate);

private:
    void close();
    bool writeBytes(const unsigned char* data, int n, juce::String& error);

    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
