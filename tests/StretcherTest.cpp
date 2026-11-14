#include "engine/Stretcher.h"
#include <catch2/catch_test_macros.hpp>
#include <limits>
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

TEST_CASE("stretcher: an output length that does not fit an int fails cleanly", "[stretcher]")
{
    // A buffer that only claims to be huge (it refers to one sample): the length check must come before any use.
    float sample = 0.0f;
    float* channels[] = { &sample };
    const int huge = std::numeric_limits<int>::max() / 2 + 1000;
    const juce::AudioBuffer<float> in(channels, 1, huge);
    CHECK_FALSE(stretchBuffer(in, kRate, 4.0).has_value());
    CHECK_FALSE(stretchBuffer(in, kRate, 2.0).has_value()); // just over INT_MAX
}

TEST_CASE("stretcher: degenerate lengths and ratios", "[stretcher]")
{
    CHECK_FALSE(stretchBuffer(noise(1, 1), kRate, 0.25).has_value()); // would round to 0 samples
    CHECK_FALSE(stretchBuffer(noise(1, 1000), kRate, std::numeric_limits<double>::quiet_NaN()).has_value());
    CHECK_FALSE(stretchBuffer(noise(1, 1000), kRate, -1.0).has_value());

    const auto empty = stretchBuffer(juce::AudioBuffer<float>(2, 0), kRate, 2.0); // nothing in, nothing out
    REQUIRE(empty.has_value());
    CHECK(empty->getNumChannels() == 2);
    CHECK(empty->getNumSamples() == 0);
}

TEST_CASE("stretchRange: lengths lie on the whole-render grid", "[stretcher]")
{
    const auto in = noise(2, 48000);
    for (double ratio : { 0.8, 1.37, 2.0 })
    {
        CAPTURE(ratio);
        // Adjoining ranges tile the whole render exactly, wherever the cuts fall.
        const std::int64_t cuts[] = { 0, 9601, 24000, 33333, 48000 };
        std::int64_t total = 0;
        for (int i = 0; i + 1 < 5; ++i)
        {
            const auto out = stretchRange(in, cuts[i], cuts[i + 1], kRate, ratio);
            REQUIRE(out.has_value());
            CHECK(out->getNumChannels() == 2);
            CHECK(out->getNumSamples() == stretchedIndex(cuts[i + 1], ratio) - stretchedIndex(cuts[i], ratio));
            total += out->getNumSamples();
        }
        CHECK(total == (std::int64_t) std::llround(48000 * ratio));
    }

    // The whole range is the whole render; a range reaching outside the buffer is clamped to it.
    const auto whole = stretchBuffer(in, kRate, 1.5);
    const auto same = stretchRange(in, -5, 48000 + 99, kRate, 1.5);
    REQUIRE(whole.has_value());
    REQUIRE(same.has_value());
    REQUIRE(same->getNumSamples() == whole->getNumSamples());
    for (int i = 0; i < whole->getNumSamples(); i += 97)
        CHECK(same->getSample(1, i) == whole->getSample(1, i));

    const auto empty = stretchRange(in, 30000, 30000, kRate, 1.5);
    REQUIRE(empty.has_value());
    CHECK(empty->getNumSamples() == 0);
    CHECK_FALSE(stretchRange(in, 100, 200, kRate, std::numeric_limits<double>::quiet_NaN()).has_value());
    CHECK_FALSE(stretchRange(in, 100, 200, kRate, 1.5, [](float) { return false; }).has_value());
}

namespace
{
// A 5 ms 2 kHz burst every half second, the first at 0.25 s.
juce::AudioBuffer<float> clickTrain(int seconds)
{
    juce::AudioBuffer<float> b(1, seconds * 48000);
    b.clear();
    for (int at = 12000; at + 240 < b.getNumSamples(); at += 24000)
        for (int i = 0; i < 240; ++i)
            b.setSample(0, at + i, 0.9f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 2000.0 * i / kRate));
    return b;
}

// The lag in [-maxLag, maxLag] at which b[i + lag] best matches a[i] over i in [0, n): 0 if they are aligned.
// Both must be readable maxLag samples beyond either end. `peak` gets the normalised correlation there.
int bestLag(const float* a, const float* b, int n, int maxLag, double& peak)
{
    double ea = 0, eb = 0;
    for (int i = 0; i < n; ++i)
    {
        ea += (double) a[i] * a[i];
        eb += (double) b[i] * b[i];
    }
    REQUIRE(ea > 0.0);
    REQUIRE(eb > 0.0);
    int best = 0;
    peak = -2.0;
    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        double sum = 0;
        for (int i = 0; i < n; ++i)
            sum += (double) a[i] * b[i + lag];
        if (sum > peak)
        {
            peak = sum;
            best = lag;
        }
    }
    peak /= std::sqrt(ea * eb);
    return best;
}
} // namespace

TEST_CASE("stretchRange: a range render is time-aligned with the whole render", "[stretcher]")
{
    const auto in = clickTrain(30);
    // 1.5 and 2.0 have analysis hops (227 and 170 samples) that divide neither range start; 0.8 has 256.
    for (double ratio : { 0.8, 1.5, 2.0 })
    {
        const auto whole = stretchBuffer(in, kRate, ratio);
        REQUIRE(whole.has_value());
        for (std::int64_t begin : { std::int64_t(10 * 48000), std::int64_t(10 * 48000 + 1234) })
        {
            CAPTURE(ratio, begin);
            const std::int64_t end = 20 * 48000;
            const auto part = stretchRange(in, begin, end, kRate, ratio);
            REQUIRE(part.has_value());
            const auto start = stretchedIndex(begin, ratio);
            REQUIRE(part->getNumSamples() == stretchedIndex(end, ratio) - start);

            // Every click at least half a second inside the range: index i of the range render is index start + i
            // of the whole render. (Unaligned analysis frames alone would move a click by tens of samples.)
            int clicks = 0;
            for (std::int64_t at = 12000; at < in.getNumSamples(); at += 24000)
            {
                if (at < begin + 24000 || at > end - 24000)
                    continue;
                const auto mid = (std::int64_t) std::llround((double) (at + 120) * ratio);
                const std::int64_t w0 = mid - 4800; // +/- 100 ms around the click
                double peak = 0;
                const int lag = bestLag(whole->getReadPointer(0) + w0, part->getReadPointer(0) + (w0 - start), 9600,
                                        300, peak);
                CAPTURE(at, lag, peak);
                CHECK(std::abs(lag) <= 2);
                CHECK(peak > 0.9);
                ++clicks;
            }
            CHECK(clicks >= 17);
        }
    }
}
