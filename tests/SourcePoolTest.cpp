#include "engine/SourcePool.h"
#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace aligna;

namespace
{
struct TempDir
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("aligna_sp_" + juce::Uuid().toString());
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
};

juce::File writeWav(const juce::File& f, const juce::AudioBuffer<float>& buf, double rate)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os(f.createOutputStream());
    REQUIRE(os != nullptr);
    std::unique_ptr<juce::AudioFormatWriter> w(
        wav.createWriterFor(os.get(), rate, (unsigned) buf.getNumChannels(), 32, {}, 0));
    REQUIRE(w != nullptr);
    os.release();
    REQUIRE(w->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples()));
    return f;
}
} // namespace

TEST_CASE("44.1k sine resampled to 48k keeps length and pitch")
{
    TempDir t;
    juce::AudioBuffer<float> b(1, 44100);
    for (int i = 0; i < 44100; ++i)
        b.setSample(0, i, std::sin(2.0 * juce::MathConstants<double>::pi * 1000.0 * i / 44100.0));
    auto f = writeWav(t.dir.getChildFile("s.wav"), b, 44100);

    SourcePool pool;
    juce::String err;
    auto info = pool.import(f, 48000, err);
    REQUIRE(info.has_value());
    REQUIRE(std::abs((int) info->length - 48000) <= 2);
    REQUIRE(info->numChannels == 1);
    REQUIRE(info->path == f.getFullPathName());
    auto d = pool.get(info->id);
    REQUIRE(d != nullptr);
    REQUIRE(d->getNumSamples() == info->length);

    std::vector<int> zc;
    auto* p = d->getReadPointer(0);
    for (int i = 20000; i < 28000; ++i)
        if (p[i] < 0.f && p[i + 1] >= 0.f)
            zc.push_back(i);
    REQUIRE(zc.size() > 10);
    for (size_t i = 1; i < zc.size(); ++i)
        REQUIRE(std::abs((zc[i] - zc[i - 1]) - 48) <= 1);
}

TEST_CASE("resampling does not shift an impulse in time")
{
    TempDir t;
    const int N = 10000;
    juce::AudioBuffer<float> b(1, 44100);
    b.clear();
    b.setSample(0, N, 1.0f);
    auto f = writeWav(t.dir.getChildFile("i.wav"), b, 44100);

    SourcePool pool;
    juce::String err;
    auto info = pool.import(f, 48000, err);
    REQUIRE(info.has_value());
    auto d = pool.get(info->id);
    auto* p = d->getReadPointer(0);
    int peak = 0;
    for (int i = 0; i < d->getNumSamples(); ++i)
        if (std::abs(p[i]) > std::abs(p[peak]))
            peak = i;
    REQUIRE(std::abs(peak - (int) std::llround(N * 48000.0 / 44100.0)) <= 1);
}

TEST_CASE("same-rate stereo file is bit exact")
{
    TempDir t;
    juce::AudioBuffer<float> b(2, 1000);
    for (int i = 0; i < 1000; ++i)
    {
        b.setSample(0, i, 0.001f * (float) i - 0.5f);
        b.setSample(1, i, -0.0005f * (float) i);
    }
    auto f = writeWav(t.dir.getChildFile("st.wav"), b, 48000);
    SourcePool pool;
    juce::String err;
    auto info = pool.import(f, 48000, err);
    REQUIRE(info.has_value());
    REQUIRE(info->numChannels == 2);
    REQUIRE(info->length == 1000);
    auto d = pool.get(info->id);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 1000; ++i)
            REQUIRE(d->getSample(c, i) == b.getSample(c, i));
    pool.clear();
    REQUIRE(pool.get(info->id) == nullptr);
}

TEST_CASE("multichannel files mix down to stereo")
{
    TempDir t;
    juce::AudioBuffer<float> b(4, 100);
    b.clear();
    for (int i = 0; i < 100; ++i)
        for (int c = 0; c < 4; ++c)
            b.setSample(c, i, c == 0 ? 0.5f : c == 1 ? 0.25f : c == 2 ? -0.25f : 0.75f);
    auto f = writeWav(t.dir.getChildFile("q.wav"), b, 48000);
    SourcePool pool;
    juce::String err;
    auto info = pool.import(f, 48000, err);
    REQUIRE(info.has_value());
    REQUIRE(info->numChannels == 2);
    auto d = pool.get(info->id);
    REQUIRE(std::abs(d->getSample(0, 10) - 0.125f) < 1e-6f);
    REQUIRE(std::abs(d->getSample(1, 10) - 0.5f) < 1e-6f);
}

TEST_CASE("errors: missing, unsupported, empty")
{
    TempDir t;
    SourcePool pool;
    juce::String err;
    REQUIRE_FALSE(pool.import(t.dir.getChildFile("nope.wav"), 48000, err).has_value());
    REQUIRE(err == "Could not open file");

    auto junk = t.dir.getChildFile("junk.wav");
    junk.replaceWithText("this is not audio at all");
    err.clear();
    REQUIRE_FALSE(pool.import(junk, 48000, err).has_value());
    REQUIRE(err == "Unsupported audio format");

    juce::AudioBuffer<float> empty(1, 0);
    auto e = writeWav(t.dir.getChildFile("empty.wav"), empty, 48000);
    err.clear();
    REQUIRE_FALSE(pool.import(e, 48000, err).has_value());
    REQUIRE(err.isNotEmpty());

    SourceInfo si{ "x", t.dir.getChildFile("gone.wav").getFullPathName(), 5, 1, false };
    REQUIRE_FALSE(pool.load(si, 48000));
    REQUIRE(si.missing);
    REQUIRE(pool.get("x") == nullptr);
}

TEST_CASE("load updates length/channels and clears missing")
{
    TempDir t;
    juce::AudioBuffer<float> b(1, 480);
    b.clear();
    auto f = writeWav(t.dir.getChildFile("a.wav"), b, 48000);
    SourcePool pool;
    SourceInfo si{ "id1", f.getFullPathName(), 1, 2, true };
    REQUIRE(pool.load(si, 48000));
    REQUIRE_FALSE(si.missing);
    REQUIRE(si.length == 480);
    REQUIRE(si.numChannels == 1);
    REQUIRE(pool.get("id1") != nullptr);
}
