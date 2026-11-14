#include "engine/Stretcher.h"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace aligna;

namespace
{
constexpr double kRate = 48000.0;

juce::AudioBuffer<float> noise(int ch, int n)
{
    juce::AudioBuffer<float> b(ch, n);
    juce::Random r(1234);
    for (int c = 0; c < ch; ++c)
        for (int i = 0; i < n; ++i)
            b.setSample(c, i, r.nextFloat() * 2.0f - 1.0f);
    return b;
}
} // namespace

TEST_CASE("stretcher: length is exact", "[stretcher]")
{
    const auto in = noise(2, 48000);
    for (double ratio : {0.25, 0.5, 0.98, 1.02, 2.0, 4.0})
    {
        const auto out = stretchBuffer(in, kRate, ratio);
        REQUIRE(out.has_value());
        CHECK(out->getNumChannels() == 2);
        CHECK(out->getNumSamples() == (int) std::llround(48000 * ratio));
    }
}

TEST_CASE("stretcher: pitch preserved", "[stretcher]")
{
    juce::AudioBuffer<float> in(1, 48000);
    for (int i = 0; i < 48000; ++i)
        in.setSample(0, i, (float) std::sin(2.0 * juce::MathConstants<double>::pi * 1000.0 * i / kRate));
    for (double ratio : {1.25, 0.8})
    {
        const auto out = stretchBuffer(in, kRate, ratio);
        REQUIRE(out.has_value());
        const int n = out->getNumSamples();
        const float* d = out->getReadPointer(0);
        int first = -1, last = -1, crossings = 0;
        for (int i = n / 4 + 1; i < 3 * n / 4; ++i)
            if ((d[i - 1] < 0.0f) != (d[i] < 0.0f)) // any zero crossing: half a period apart
            {
                if (first < 0)
                    first = i;
                last = i;
                ++crossings;
            }
        REQUIRE(crossings > 10);
        const double spacing = double(last - first) / (crossings - 1);
        CHECK(std::abs(spacing - 24.0) < 0.5);
    }
}

TEST_CASE("stretcher: timing preserved", "[stretcher]")
{
    juce::AudioBuffer<float> in(1, 48000);
    in.clear();
    const int clickAt = 24000, clickLen = 240; // 5 ms
    for (int i = 0; i < clickLen; ++i)
        in.setSample(0, clickAt + i, 0.9f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 2000.0 * i / kRate));
    const auto out = stretchBuffer(in, kRate, 2.0);
    REQUIRE(out.has_value());
    // The R2 phase vocoder smears a lone click over ~20 ms, so locate it by its energy centroid (the burst's
    // centre, 0.5 s + 2.5 ms, scaled by the ratio) rather than by a first-sample threshold.
    const float* d = out->getReadPointer(0);
    double e = 0, m = 0;
    for (int i = 0; i < out->getNumSamples(); ++i)
    {
        const double p = (double) d[i] * d[i];
        e += p;
        m += p * i;
    }
    REQUIRE(e > 0.0);
    const double expected = 2.0 * (clickAt + clickLen / 2.0);
    CHECK(std::abs(m / e - expected) <= 480); // 10 ms; an uncompensated R2 start delay would be >20 ms
}

TEST_CASE("stretcher: cancel", "[stretcher]")
{
    const auto in = noise(2, 48000);
    const auto out = stretchBuffer(in, kRate, 1.5, [](float) { return false; });
    CHECK_FALSE(out.has_value());
}

TEST_CASE("stretcher: mono stays mono", "[stretcher]")
{
    const auto out = stretchBuffer(noise(1, 24000), kRate, 1.5);
    REQUIRE(out.has_value());
    CHECK(out->getNumChannels() == 1);
}

TEST_CASE("stretcher: benchmark 60s stereo", "[.benchmark]")
{
    const auto in = noise(2, 48000 * 60);
    const auto t0 = std::chrono::steady_clock::now();
    const auto out = stretchBuffer(in, kRate, 1.05);
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(out.has_value());
    std::printf("stretch 60 s stereo at 1.05: %.0f ms\n", ms);
}
