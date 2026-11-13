#include "align/AlignClips.h"
#include "align/Correlate.h"
#include <catch2/catch_test_macros.hpp>

using namespace aligna;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kLen = 5 * 48000;

std::vector<float> noise(int n, juce::Random& r, float amp = 0.5f)
{
    std::vector<float> v((size_t) n);
    for (auto& x : v)
        x = amp * (r.nextFloat() * 2.0f - 1.0f);
    return v;
}

// target[n] = sign * reference[n - delay] (zero where the reference index is out of range).
std::vector<float> delayed(const std::vector<float>& ref, int delay, float sign = 1.0f)
{
    std::vector<float> t(ref.size(), 0.0f);
    for (int n = 0; n < (int) ref.size(); ++n)
    {
        const int k = n - delay;
        if (k >= 0 && k < (int) ref.size())
            t[(size_t) n] = sign * ref[(size_t) k];
    }
    return t;
}

const std::function<bool(float)> noProgress = {};
} // namespace

TEST_CASE("findOffset finds exact lag for delayed noise")
{
    juce::Random r(42);
    auto ref = noise(kLen, r);
    for (int delay : { 12345, -777, 0 })
    {
        CAPTURE(delay);
        juce::String err;
        auto res = findOffset(ref, delayed(ref, delay), 48000, kRate, noProgress, err);
        REQUIRE(res.has_value());
        CHECK(res->lag == delay);
        CHECK_FALSE(res->polarityInverted);
        CHECK(res->confidence >= kConfidentThreshold);
    }
}

TEST_CASE("findOffset survives -10 dB SNR noise")
{
    juce::Random r(42);
    auto ref = noise(kLen, r, 0.1f);
    auto tgt = delayed(ref, 12345);
    // Signal RMS = 0.1/sqrt(3); noise 10 dB louder => amplitude * sqrt(10).
    auto extra = noise(kLen, r, 0.1f * std::sqrt(10.0f));
    for (size_t i = 0; i < tgt.size(); ++i)
        tgt[i] += extra[i];
    juce::String err;
    auto res = findOffset(ref, tgt, 48000, kRate, noProgress, err);
    REQUIRE(res.has_value());
    CHECK(res->lag == 12345);
    CHECK(res->confidence >= kConfidentThreshold);
}

TEST_CASE("findOffset reports inverted polarity")
{
    juce::Random r(42);
    auto ref = noise(kLen, r);
    juce::String err;
    auto res = findOffset(ref, delayed(ref, 12345, -1.0f), 48000, kRate, noProgress, err);
    REQUIRE(res.has_value());
    CHECK(res->lag == 12345);
    CHECK(res->polarityInverted);
}

TEST_CASE("findOffset on unrelated signals has low confidence")
{
    juce::Random r(42);
    auto a = noise(kLen, r);
    auto b = noise(kLen, r);
    juce::String err;
    auto res = findOffset(a, b, 48000, kRate, noProgress, err);
    REQUIRE(res.has_value());
    CHECK(res->confidence < kConfidentThreshold);
}

TEST_CASE("findOffset rejects a silent signal")
{
    juce::Random r(42);
    auto ref = noise(kLen, r);
    std::vector<float> silent((size_t) kLen, 0.0f);
    juce::String err;
    auto res = findOffset(ref, silent, 48000, kRate, noProgress, err);
    CHECK_FALSE(res.has_value());
    CHECK(err == "Clip is silent in the compared region");
}

TEST_CASE("findOffset cancels when progress returns false")
{
    juce::Random r(42);
    auto ref = noise(kLen, r);
    juce::String err;
    int calls = 0;
    auto res = findOffset(ref, delayed(ref, 100), 48000, kRate,
                          [&](float) { ++calls; return false; }, err);
    CHECK_FALSE(res.has_value());
    CHECK(err == "Cancelled");
    CHECK(calls == 1);
}

TEST_CASE("findOffset reports increasing progress")
{
    juce::Random r(42);
    auto ref = noise(kLen, r);
    juce::String err;
    std::vector<float> ps;
    auto res = findOffset(ref, delayed(ref, 100), 48000, kRate,
                          [&](float p) { ps.push_back(p); return true; }, err);
    REQUIRE(res.has_value());
    REQUIRE(ps.size() >= 10);
    for (size_t i = 1; i < ps.size(); ++i)
        CHECK(ps[i] >= ps[i - 1]);
    CHECK(ps.front() >= 0.0f);
    CHECK(ps.back() >= 0.999f);
}

namespace
{
MixSnapshot twoClipSnapshot(SamplePos targetStart)
{
    juce::Random r(42);
    auto buf = std::make_shared<juce::AudioBuffer<float>>(2, 8 * 48000);
    for (int i = 0; i < buf->getNumSamples(); ++i)
    {
        const float v = r.nextFloat() * 2.0f - 1.0f;
        buf->setSample(0, i, v);
        buf->setSample(1, i, 0.5f * v);
    }
    MixSnapshot s;
    s.project.sampleRate = kRate;
    SourceInfo si;
    si.id = "src";
    si.length = buf->getNumSamples();
    si.numChannels = 2;
    s.project.sources.push_back(si);
    s.audio["src"] = buf;

    Track t1, t2;
    Clip ref;
    ref.id = "ref";
    ref.sourceId = "src";
    ref.start = 0;
    ref.sourceOffset = 0;
    ref.length = 6 * 48000;
    t1.clips.push_back(ref);

    // Content starts 1 s into the source, so it belongs at timeline 48000.
    Clip tgt;
    tgt.id = "tgt";
    tgt.sourceId = "src";
    tgt.start = targetStart;
    tgt.sourceOffset = 48000;
    tgt.length = 3 * 48000;
    tgt.gainDb = -6.0f;
    t2.clips.push_back(tgt);
    s.project.tracks = { t1, t2 };
    return s;
}
} // namespace

TEST_CASE("computeAlignment lines up a late clip to the exact sample")
{
    auto s = twoClipSnapshot(48000 + 24000); // 0.5 s later than its content requires
    juce::String err;
    auto res = computeAlignment(s, "ref", "tgt", std::nullopt, 30.0, noProgress, err);
    REQUIRE(res.has_value());
    CHECK(res->newStart == 48000);
    CHECK_FALSE(res->polarityInverted);
    CHECK(res->confidence >= kConfidentThreshold);
}

TEST_CASE("computeAlignment with a selection")
{
    auto s = twoClipSnapshot(48000 - 3000); // early clip
    juce::String err;
    auto res = computeAlignment(s, "ref", "tgt", juce::Range<SamplePos>(60000, 150000), 2.0, noProgress, err);
    REQUIRE(res.has_value());
    CHECK(res->newStart == 48000);
}

TEST_CASE("computeAlignment errors")
{
    auto s = twoClipSnapshot(48000);
    juce::String err;
    CHECK_FALSE(computeAlignment(s, "ref", "nope", std::nullopt, 30.0, noProgress, err).has_value());
    CHECK(err == "Clip not found");
    CHECK_FALSE(computeAlignment(s, "ref", "ref", std::nullopt, 30.0, noProgress, err).has_value());
    CHECK(err == "Choose two different clips");

    auto far = twoClipSnapshot(100 * 48000);
    CHECK_FALSE(computeAlignment(far, "ref", "tgt", std::nullopt, 30.0, noProgress, err).has_value());
    CHECK(err == juce::String::fromUTF8("Clips are too far apart \xe2\x80\x94 increase the search range"));
}

TEST_CASE("computeAlignment: a stretched clip without its buffer is still stretching")
{
    const auto stretching = juce::String::fromUTF8("Clip is still stretching \xe2\x80\x94 try again in a moment");
    for (int which = 0; which < 2; ++which)
    {
        CAPTURE(which);
        auto s = twoClipSnapshot(48000);
        auto& c = s.project.tracks[(size_t) which].clips[0];
        c.stretch = 1.5; // its buffer "src@1.5" is not in the snapshot
        juce::String err;
        CHECK_FALSE(computeAlignment(s, "ref", "tgt", std::nullopt, 30.0, noProgress, err).has_value());
        CHECK(err == stretching);

        // Once the buffer is there, alignment runs (the content is the plain source here, so it still lines up).
        s.audio[bufferKey(c)] = s.audio["src"];
        err.clear();
        auto res = computeAlignment(s, "ref", "tgt", std::nullopt, 30.0, noProgress, err);
        REQUIRE(res.has_value());
        CHECK(res->newStart == 48000);
    }
}
