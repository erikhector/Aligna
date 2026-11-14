#include "align/AlignClips.h"
#include "align/Correlate.h"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <limits>

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

TEST_CASE("computeAlignment: a stretched clip with a missing source takes the missing-source path")
{
    // Unstretched missing source: the clip renders silent.
    auto base = twoClipSnapshot(48000);
    base.project.sources[0].missing = true;
    base.audio.clear();
    juce::String missingErr;
    CHECK_FALSE(computeAlignment(base, "ref", "tgt", std::nullopt, 30.0, noProgress, missingErr).has_value());

    for (bool nullSource : { false, true })
    {
        CAPTURE(nullSource);
        auto s = twoClipSnapshot(48000);
        s.audio.clear();
        if (nullSource)
            s.project.sources.clear();
        else
            s.project.sources[0].missing = true;
        s.project.tracks[1].clips[0].stretch = 1.5;
        juce::String err;
        CHECK_FALSE(computeAlignment(s, "ref", "tgt", std::nullopt, 30.0, noProgress, err).has_value());
        CHECK(err == missingErr);
        CHECK(err.isNotEmpty());
        CHECK(! err.contains("stretching"));
    }
}

TEST_CASE("findOffset gives no result for non-finite input")
{
    juce::Random r(42);
    const auto ref = noise(kLen, r);
    const auto tgt = delayed(ref, 1200);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    for (float bad : { nan, inf, -inf })
        for (int which = 0; which < 2; ++which)
            for (bool all : { false, true })
            {
                CAPTURE(bad, which, all);
                auto a = ref, b = tgt;
                auto& v = which == 0 ? a : b;
                if (all)
                    std::fill(v.begin(), v.end(), bad);
                else
                    v[v.size() / 2] = bad;
                juce::String err;
                const auto res = findOffset(a, b, 48000, kRate, noProgress, err);
                CHECK_FALSE(res.has_value());
                CHECK(err == "Clip contains invalid audio in the compared region");
            }
}

TEST_CASE("computeAlignment: a start before the project start is reported unclamped")
{
    // The reference shows the source from 1 s on at timeline 0, so the target's first sample (source 0) belongs
    // at timeline -48000.
    auto s = twoClipSnapshot(24000);
    s.project.tracks[0].clips[0].sourceOffset = 48000;
    s.project.tracks[1].clips[0].sourceOffset = 0;
    juce::String err;
    auto res = computeAlignment(s, "ref", "tgt", std::nullopt, 30.0, noProgress, err);
    REQUIRE(res.has_value());
    CHECK(res->newStart == -48000);
    CHECK(res->confidence >= kConfidentThreshold);
    CHECK(res->newStart + s.project.tracks[1].clips[0].length > 0); // part of the clip remains on the timeline
}

TEST_CASE("computeAlignment: a clip whose stretch failed says so instead of 'still stretching'")
{
    for (int which = 0; which < 2; ++which)
    {
        CAPTURE(which);
        auto s = twoClipSnapshot(48000);
        auto& c = s.project.tracks[(size_t) which].clips[0];
        c.stretch = 1.5;
        s.failedStretch.insert(bufferKey(c));
        juce::String err;
        CHECK_FALSE(computeAlignment(s, "ref", "tgt", std::nullopt, 30.0, noProgress, err).has_value());
        CHECK(err == "Clip could not be stretched, so it has no audio to compare");
    }
}

namespace
{
// Progress callback that remembers the values it is given and returns false on call number `cancelAt`
// (0 = never cancels).
struct CancelAfter
{
    explicit CancelAfter(int n) : cancelAt(n) {}
    std::function<bool(float)> callback()
    {
        return [this](float p) {
            seen.push_back(p);
            return (int) seen.size() != cancelAt;
        };
    }
    int calls() const { return (int) seen.size(); }
    int cancelAt;
    std::vector<float> seen;
};
} // namespace

TEST_CASE("renderClipAlone: the block size does not change the result")
{
    const auto s = twoClipSnapshot(48000 + 24000);
    const auto& clip = s.project.tracks[1].clips[0];
    const auto& buf = *s.audio.at("src");
    // Starts before the clip and ends after it, so both silent margins are covered.
    const SamplePos from = clip.start - 5000, n = clip.length + 12345;
    const int first = 48000, last = (int) (48000 + clip.length - 1); // source samples at the clip's two ends

    const auto whole = renderClipAlone(s, clip, from, n, {}, std::numeric_limits<SamplePos>::max());
    REQUIRE(whole.has_value());
    REQUIRE((SamplePos) whole->size() == n);
    CHECK(juce::exactlyEqual((*whole)[0], 0.0f));
    CHECK(juce::exactlyEqual((*whole)[4999], 0.0f));
    CHECK(juce::exactlyEqual((*whole)[5000], (buf.getSample(0, first) + buf.getSample(1, first)) * 0.5f));
    CHECK(juce::exactlyEqual((*whole)[(size_t) (5000 + clip.length - 1)], (buf.getSample(0, last) + buf.getSample(1, last)) * 0.5f));
    CHECK(juce::exactlyEqual((*whole)[(size_t) (5000 + clip.length)], 0.0f));

    for (SamplePos block : { (SamplePos) 1, (SamplePos) 7, (SamplePos) 1000, kAlignRenderBlock, clip.length, (SamplePos) 0 })
    {
        CAPTURE(block);
        CancelAfter never(0);
        const auto chunked = renderClipAlone(s, clip, from, n, never.callback(), block);
        REQUIRE(chunked.has_value());
        CHECK(*chunked == *whole);
        // Asked once before each block (a block size below 1 counts as 1).
        const SamplePos size = std::max<SamplePos>(1, block);
        CHECK(never.calls() == (int) ((clip.length + size - 1) / size));
    }
    CHECK(renderClipAlone(s, clip, from, n) == whole); // default block size, no callback
}

TEST_CASE("renderClipAlone stops at the block where cancellation is requested")
{
    const auto s = twoClipSnapshot(48000);
    const auto& clip = s.project.tracks[0].clips[0]; // 6 s = 288 blocks of 1000
    for (int cancelAt : { 1, 2, 100, 288 })
    {
        CAPTURE(cancelAt);
        CancelAfter c(cancelAt);
        CHECK_FALSE(renderClipAlone(s, clip, 0, clip.length, c.callback(), 1000).has_value());
        CHECK(c.calls() == cancelAt); // not asked again, so no further block was rendered
        CHECK(juce::exactlyEqual(c.seen.back(), (float) (cancelAt - 1) / 288.0f));
    }
}

TEST_CASE("findOffset: progress checks are frequent and do not change the result")
{
    juce::Random r(42);
    const auto ref = noise(kLen, r);
    const auto tgt = delayed(ref, 12345);
    juce::String err;
    const auto plain = findOffset(ref, tgt, 48000, kRate, noProgress, err);
    REQUIRE(plain.has_value());

    CancelAfter never(0);
    const auto res = findOffset(ref, tgt, 48000, kRate, never.callback(), err);
    REQUIRE(res.has_value());
    CHECK(res->lag == plain->lag);
    CHECK(juce::exactlyEqual(res->confidence, plain->confidence));
    CHECK(res->polarityInverted == plain->polarityInverted);

    // The long passes ask at least every 65536 samples, not once per pass.
    auto callsIn = [&](float lo, float hi) {
        return (int) std::count_if(never.seen.begin(), never.seen.end(), [&](float p) { return p >= lo && p < hi; });
    };
    CHECK(callsIn(0.0f, 0.02f) >= 2 * (kLen / 65536)); // RMS of both signals
    CHECK(callsIn(0.02f, 0.05f) >= 2);                 // decimation
    CHECK(callsIn(0.35f, 0.45f) >= 3);                 // spectrum normalisation (65537 bins here)
    CHECK(callsIn(0.6f, 0.65f) >= 3);                  // the two peak scans
    CHECK(callsIn(0.65f, 1.01f) >= 10);                // fine pass
}

TEST_CASE("findOffset: cancelling at any point stops at once")
{
    juce::Random r(42);
    const auto ref = noise(kLen, r);
    const auto tgt = delayed(ref, 12345);
    juce::String err;
    CancelAfter never(0);
    REQUIRE(findOffset(ref, tgt, 48000, kRate, never.callback(), err).has_value());
    const int total = never.calls();
    REQUIRE(total >= 30);

    for (int cancelAt = 1; cancelAt <= total; ++cancelAt)
    {
        CAPTURE(cancelAt);
        CancelAfter c(cancelAt);
        err.clear();
        CHECK_FALSE(findOffset(ref, tgt, 48000, kRate, c.callback(), err).has_value());
        CHECK(err == "Cancelled");
        // Never asked again after the refusal, so nothing beyond the block or transform in hand was done; and it
        // had got exactly as far as the uncancelled run at that call.
        REQUIRE(c.calls() == cancelAt);
        CHECK(juce::exactlyEqual(c.seen.back(), never.seen[(size_t) cancelAt - 1]));
    }
}

TEST_CASE("computeAlignment: cancelling mid-render or mid-correlation returns Cancelled at once")
{
    const auto s = twoClipSnapshot(48000 + 24000);
    juce::String err;
    const auto plain = computeAlignment(s, "ref", "tgt", std::nullopt, 2.0, noProgress, err);
    REQUIRE(plain.has_value());

    CancelAfter never(0);
    const auto res = computeAlignment(s, "ref", "tgt", std::nullopt, 2.0, never.callback(), err);
    REQUIRE(res.has_value());
    CHECK(res->newStart == plain->newStart);
    CHECK(juce::exactlyEqual(res->confidence, plain->confidence));
    CHECK(res->polarityInverted == plain->polarityInverted);
    for (size_t i = 1; i < never.seen.size(); ++i)
        CHECK(never.seen[i] >= never.seen[i - 1]);
    CHECK(never.seen.back() >= 0.999f);

    // The first tenth of the progress is rendering: the 6 s reference region alone is asked once per second.
    const int total = never.calls();
    const int renderCalls =
        (int) std::count_if(never.seen.begin(), never.seen.end(), [](float p) { return p < 0.1f; });
    REQUIRE(renderCalls >= 6);
    REQUIRE(total > renderCalls + 20);

    bool cancelledInRender = false, cancelledInCorrelation = false;
    for (int cancelAt = 1; cancelAt <= total; ++cancelAt)
    {
        CAPTURE(cancelAt);
        CancelAfter c(cancelAt);
        err.clear();
        CHECK_FALSE(computeAlignment(s, "ref", "tgt", std::nullopt, 2.0, c.callback(), err).has_value());
        CHECK(err == "Cancelled");
        REQUIRE(c.calls() == cancelAt);
        CHECK(juce::exactlyEqual(c.seen.back(), never.seen[(size_t) cancelAt - 1]));
        if (c.seen.back() < 0.1f)
            cancelledInRender = true;
        else
            cancelledInCorrelation = true;
    }
    CHECK(cancelledInRender);
    CHECK(cancelledInCorrelation);
}

TEST_CASE("computeAlignment: a stretched clip whose buffer holds only part of the source")
{
    // The target's buffer "src@1.5" holds the stretched-source samples from `first` on only (here with the plain
    // source's content, as above); the clip plays [48000, 192000) of it.
    const auto partOf = [](const MixSnapshot& s, int first, int length) {
        const auto& src = *s.audio.at("src");
        auto part = std::make_shared<juce::AudioBuffer<float>>(src.getNumChannels(), length);
        for (int ch = 0; ch < src.getNumChannels(); ++ch)
            part->copyFrom(ch, 0, src, ch, first, length);
        return part;
    };

    SECTION("the buffer's offset is applied")
    {
        auto s = twoClipSnapshot(48000 + 24000);
        auto& c = s.project.tracks[1].clips[0];
        c.stretch = 1.5;
        s.audio[bufferKey(c)] = partOf(s, 40000, 160000);
        s.audioStart[bufferKey(c)] = 40000;
        juce::String err;
        auto res = computeAlignment(s, "ref", "tgt", std::nullopt, 30.0, noProgress, err);
        REQUIRE(res.has_value());
        CHECK(res->newStart == 48000);
        CHECK(res->confidence >= kConfidentThreshold);
    }
    SECTION("what the buffer does not hold reads as silence")
    {
        auto s = twoClipSnapshot(48000 + 24000);
        auto& c = s.project.tracks[1].clips[0];
        c.stretch = 1.5;
        s.audio[bufferKey(c)] = partOf(s, 100000, 50000); // starts after the clip's first sample, ends before its last
        s.audioStart[bufferKey(c)] = 100000;
        juce::String err;
        auto res = computeAlignment(s, "ref", "tgt", std::nullopt, 30.0, noProgress, err);
        REQUIRE(res.has_value());
        CHECK(res->newStart == 48000);
    }
    SECTION("a buffer still being rendered larger is 'still stretching'")
    {
        for (int which = 0; which < 2; ++which)
        {
            CAPTURE(which);
            auto s = twoClipSnapshot(48000);
            auto& c = s.project.tracks[(size_t) which].clips[0];
            c.stretch = 1.5;
            s.audio[bufferKey(c)] = s.audio["src"];
            s.partialStretch.insert(bufferKey(c));
            juce::String err;
            CHECK_FALSE(computeAlignment(s, "ref", "tgt", std::nullopt, 30.0, noProgress, err).has_value());
            CHECK(err == juce::String::fromUTF8("Clip is still stretching \xe2\x80\x94 try again in a moment"));
        }
    }
}
