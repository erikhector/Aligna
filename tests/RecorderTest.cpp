#include "engine/Recorder.h"
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>
#include <thread>

using namespace aligna;

namespace
{
juce::File tempWav()
{
    return juce::File::createTempFile(".wav");
}

struct Wav
{
    juce::AudioBuffer<float> data;
    bool ok = false;
};

Wav readWav(const juce::File& f)
{
    Wav w;
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::AudioFormatReader> r(fmt.createReaderFor(f.createInputStream().release(), true));
    if (r == nullptr)
        return w;
    w.data.setSize((int) r->numChannels, (int) r->lengthInSamples);
    r->read(&w.data, 0, (int) r->lengthInSamples, 0, true, true);
    w.ok = true;
    return w;
}

float sine(int i) { return 0.5f * std::sin(2.0f * juce::MathConstants<float>::pi * 440.0f * (float) i / 48000.0f); }

void feedSine(Recorder& r, int blocks, int len, int startIndex = 0)
{
    std::vector<float> b((size_t) len);
    for (int k = 0; k < blocks; ++k)
    {
        for (int i = 0; i < len; ++i)
            b[(size_t) i] = sine(startIndex + k * len + i);
        const float* ch[1] = {b.data()};
        r.process(ch, 1, len);
    }
}
} // namespace

TEST_CASE("records mono input exactly", "[recorder]")
{
    auto f = tempWav();
    Recorder r;
    juce::String err;
    REQUIRE(r.open(f, 48000.0, 1, err));
    r.begin();
    feedSine(r, 10, 480);
    auto res = r.close();
    REQUIRE(res.numSamples == 4800);
    REQUIRE(res.deviceRate == 48000.0);
    auto w = readWav(f);
    REQUIRE(w.ok);
    REQUIRE(w.data.getNumSamples() == 4800);
    for (int i = 0; i < 4800; ++i)
        REQUIRE(std::abs(w.data.getSample(0, i) - sine(i)) < 1e-5f);
    f.deleteFile();
}

TEST_CASE("stereo takes the first two inputs", "[recorder]")
{
    auto f = tempWav();
    Recorder r;
    juce::String err;
    REQUIRE(r.open(f, 48000.0, 2, err));
    r.begin();
    std::vector<float> l(480, 0.25f), rr(480, -0.25f), x(480, 0.9f);
    const float* ch[3] = {l.data(), rr.data(), x.data()};
    r.process(ch, 3, 480);
    r.close();
    auto w = readWav(f);
    REQUIRE(w.data.getNumChannels() == 2);
    REQUIRE(w.data.getNumSamples() == 480);
    REQUIRE(std::abs(w.data.getSample(0, 10) - 0.25f) < 1e-5f);
    REQUIRE(std::abs(w.data.getSample(1, 10) + 0.25f) < 1e-5f);
    f.deleteFile();
}

TEST_CASE("nothing before begin", "[recorder]")
{
    auto f = tempWav();
    Recorder r;
    juce::String err;
    REQUIRE(r.open(f, 48000.0, 1, err));
    feedSine(r, 5, 480);
    REQUIRE_FALSE(r.isCapturing());
    r.begin();
    REQUIRE(r.isCapturing());
    feedSine(r, 5, 480);
    auto res = r.close();
    REQUIRE(res.numSamples == 2400);
    REQUIRE(readWav(f).data.getNumSamples() == 2400);
    f.deleteFile();
}

TEST_CASE("missing channel is silent", "[recorder]")
{
    auto f = tempWav();
    Recorder r;
    juce::String err;
    REQUIRE(r.open(f, 48000.0, 2, err));
    r.begin();
    feedSine(r, 4, 480);
    r.close();
    auto w = readWav(f);
    REQUIRE(w.data.getNumChannels() == 2);
    REQUIRE(w.data.getNumSamples() == 1920);
    REQUIRE(w.data.getMagnitude(0, 0, 1920) > 0.4f);
    REQUIRE(w.data.getMagnitude(1, 0, 1920) == 0.0f);
    f.deleteFile();
}

TEST_CASE("peak meter", "[recorder]")
{
    Recorder r;
    feedSine(r, 10, 480);
    REQUIRE(std::abs(r.getAndResetPeak() - 0.5f) < 0.01f);
    REQUIRE(r.getAndResetPeak() == 0.0f);
}

TEST_CASE("header is flushed while open", "[recorder]")
{
    auto f = tempWav();
    Recorder r;
    juce::String err;
    REQUIRE(r.open(f, 48000.0, 1, err));
    r.begin();
    for (int k = 0; k < 200; ++k) // 2 s in 480-sample blocks
    {
        feedSine(r, 1, 480, k * 480);
        juce::Thread::sleep(1);
    }
    bool seen = false;
    for (int t = 0; t < 60 && ! seen; ++t)
    {
        juce::WavAudioFormat fmt;
        std::unique_ptr<juce::AudioFormatReader> rd(fmt.createReaderFor(f.createInputStream().release(), true));
        seen = rd != nullptr && rd->lengthInSamples >= 48000;
        if (! seen)
            juce::Thread::sleep(50);
    }
    REQUIRE(seen);
    REQUIRE(r.isOpen());
    r.close();
    f.deleteFile();
}

TEST_CASE("overflow fails but keeps the file", "[recorder]")
{
    auto f = tempWav();
    Recorder r(1024);
    juce::String err;
    REQUIRE(r.open(f, 48000.0, 1, err));
    r.begin();
    std::vector<float> b(8192, 0.1f);
    const float* ch[1] = {b.data()};
    r.process(ch, 1, 8192);
    REQUIRE(r.hasFailed());
    REQUIRE_FALSE(r.isCapturing());
    REQUIRE(r.failureMessage() == "Recording stopped: the disk could not keep up.");
    r.close();
    REQUIRE(readWav(f).ok);
    f.deleteFile();
}

TEST_CASE("deviceStopped fails", "[recorder]")
{
    Recorder r;
    REQUIRE_FALSE(r.hasFailed());
    r.deviceStopped();
    REQUIRE(r.hasFailed());
    REQUIRE(r.failureMessage() == "Recording stopped: the audio device stopped or changed.");
}
