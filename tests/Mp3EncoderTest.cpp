#include <catch2/catch_test_macros.hpp>
#include <juce_audio_formats/juce_audio_formats.h>
#include "io/Mp3Encoder.h"
#include <cmath>

using namespace aligna;

namespace
{
juce::AudioBuffer<float> makeTone(double rate, int n, float freq, float amp)
{
    juce::AudioBuffer<float> b(2, n);
    for (int i = 0; i < n; ++i)
    {
        const auto v = amp * (float) std::sin(2.0 * juce::MathConstants<double>::pi * freq * i / rate);
        b.setSample(0, i, v);
        b.setSample(1, i, v);
    }
    return b;
}

bool encode(const juce::File& f, double rate, int kbps, const juce::String& title,
            const juce::AudioBuffer<float>& buf, juce::String& err)
{
    Mp3Encoder enc;
    if (! enc.open(f, rate, kbps, title, err)) return false;
    const int n = buf.getNumSamples();
    for (int pos = 0; pos < n; pos += 4096)
    {
        const int len = juce::jmin(4096, n - pos);
        juce::AudioBuffer<float> blk(2, len);
        for (int c = 0; c < 2; ++c) blk.copyFrom(c, 0, buf, c, pos, len);
        if (! enc.write(blk, len, err)) return false;
    }
    return enc.finish(err);
}

std::unique_ptr<juce::AudioFormatReader> openMp3(const juce::File& f)
{
    juce::MP3AudioFormat fmt;
    return std::unique_ptr<juce::AudioFormatReader>(fmt.createReaderFor(new juce::FileInputStream(f), true));
}
}

TEST_CASE("mp3 outputRateFor", "[mp3]")
{
    REQUIRE(Mp3Encoder::outputRateFor(44100) == 44100);
    REQUIRE(Mp3Encoder::outputRateFor(48000) == 48000);
    REQUIRE(Mp3Encoder::outputRateFor(32000) == 32000);
    REQUIRE(Mp3Encoder::outputRateFor(96000) == 44100);
    REQUIRE(Mp3Encoder::outputRateFor(22050) == 44100);
}

TEST_CASE("mp3 rejects bad bitrate", "[mp3]")
{
    juce::TemporaryFile tmp(".mp3");
    Mp3Encoder enc;
    juce::String err;
    REQUIRE_FALSE(enc.open(tmp.getFile(), 44100, 128, "x", err));
    REQUIRE(err.isNotEmpty());
}

TEST_CASE("mp3 encodes 1 kHz tone", "[mp3]")
{
    juce::TemporaryFile tmp(".mp3");
    juce::String err;
    REQUIRE(encode(tmp.getFile(), 48000, 320, "t", makeTone(48000, 96000, 1000.f, 0.5f), err));
    auto r = openMp3(tmp.getFile());
    REQUIRE(r != nullptr);
    REQUIRE((int) r->sampleRate == 48000);
    REQUIRE(std::abs((int) r->lengthInSamples - 96000) <= 2304);

    juce::AudioBuffer<float> out(2, 48000);
    r->read(&out, 0, 48000, 24000, true, true);
    int crossings = 0;
    for (int i = 1; i < 48000; ++i)
        if ((out.getSample(0, i - 1) < 0.f) != (out.getSample(0, i) < 0.f)) ++crossings;
    REQUIRE(std::abs(crossings - 2000) <= 20);
}

TEST_CASE("mp3 96k input is resampled to 44.1k", "[mp3]")
{
    juce::TemporaryFile tmp(".mp3");
    juce::String err;
    REQUIRE(encode(tmp.getFile(), 96000, 320, "t", makeTone(96000, 96000, 1000.f, 0.5f), err));
    auto r = openMp3(tmp.getFile());
    REQUIRE(r != nullptr);
    REQUIRE((int) r->sampleRate == 44100);
    REQUIRE(std::abs((int) r->lengthInSamples - 44100) <= 2304);
}

TEST_CASE("mp3 tiny input", "[mp3]")
{
    juce::TemporaryFile tmp(".mp3");
    juce::String err;
    REQUIRE(encode(tmp.getFile(), 44100, 192, "t", makeTone(44100, 100, 1000.f, 0.5f), err));
    REQUIRE(openMp3(tmp.getFile()) != nullptr);
}

TEST_CASE("mp3 title tag", "[mp3]")
{
    juce::TemporaryFile tmp(".mp3");
    juce::String err;
    const juce::String title = juce::String(juce::CharPointer_UTF8("Mix \xC3\xB6 1"));
    REQUIRE(encode(tmp.getFile(), 44100, 256, title, makeTone(44100, 44100, 440.f, 0.3f), err));

    juce::MemoryBlock mb;
    tmp.getFile().loadFileAsData(mb);
    REQUIRE(mb.getSize() > 256);
    const auto* d = static_cast<const unsigned char*>(mb.getData());
    REQUIRE(d[0] == 'I'); REQUIRE(d[1] == 'D'); REQUIRE(d[2] == '3');

    auto contains = [&](const std::string& needle)
    {
        return std::string((const char*) d, 256).find(needle) != std::string::npos;
    };
    REQUIRE((contains("Mix \xC3\xB6 1") || contains("Mix \xF6 1")));
    REQUIRE(openMp3(tmp.getFile()) != nullptr);
}
