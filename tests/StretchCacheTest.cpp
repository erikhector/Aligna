#include "engine/StretchCache.h"
#include "engine/Stretcher.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <atomic>
#include <new>
#include <juce_events/juce_events.h>
#include <cmath>
#include <utility>

using namespace aligna;

namespace
{
struct TempDir
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("aligna_sc_" + juce::Uuid().toString());
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
};

// One second of a 440 Hz sine, imported into `pool` at 48 kHz and added to `p` with one clip at `stretch`.
Clip addSineSource(Project& p, SourcePool& pool, const juce::File& dir, double stretch, int len = 48000)
{
    juce::AudioBuffer<float> b(1, len);
    for (int i = 0; i < len; ++i)
        b.setSample(0, i, 0.5f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / 48000.0));
    const auto f = dir.getChildFile(juce::Uuid().toString() + ".wav");
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os(f.createOutputStream());
        REQUIRE(os != nullptr);
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), 48000, 1, 32, {}, 0));
        REQUIRE(w != nullptr);
        os.release();
        REQUIRE(w->writeFromAudioSampleBuffer(b, 0, len));
    }
    juce::String err;
    auto info = pool.import(f, 48000, err);
    REQUIRE(info.has_value());
    p.sources.push_back(*info);

    Clip c;
    c.id = newId();
    c.sourceId = info->id;
    c.stretch = stretch;
    c.length = (SamplePos) std::llround((double) info->length * stretch);
    Track t;
    t.id = newId();
    t.clips.push_back(c);
    p.tracks.push_back(t);
    return c;
}

Project withStretch(Project p, double stretch)
{
    p.tracks[0].clips[0].stretch = stretch;
    return p;
}

int expectedLen(double stretch, int len = 48000) { return (int) std::llround((double) len * stretch); }
} // namespace

TEST_CASE("StretchCache renders required keys", "[stretchcache]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir t;
    SourcePool pool;
    Project p;
    const auto clip = addSineSource(p, pool, t.dir, 2.0);

    StretchCache cache;
    int ready = 0;
    cache.onReady = [&] { ++ready; };
    cache.require(p, pool);
    CHECK(cache.isPending(bufferKey(clip)));
    cache.waitForIdle();

    auto d = cache.get(bufferKey(clip));
    REQUIRE(d != nullptr);
    CHECK(d->getNumSamples() == expectedLen(2.0));
    CHECK(d->getNumChannels() == 1);
    CHECK_FALSE(cache.isPending(bufferKey(clip)));
    CHECK(ready >= 1);
    CHECK(cache.bytesUsed() == (size_t) expectedLen(2.0) * sizeof(float));

    // Requiring again with the buffer ready queues nothing.
    cache.require(p, pool);
    CHECK_FALSE(cache.isPending(bufferKey(clip)));
}

TEST_CASE("StretchCache: ratio 1 is never rendered", "[stretchcache]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir t;
    SourcePool pool;
    Project p;
    const auto clip = addSineSource(p, pool, t.dir, 1.0);

    StretchCache cache;
    cache.require(p, pool);
    CHECK_FALSE(cache.isPending(bufferKey(clip)));
    cache.waitForIdle();
    CHECK(cache.bytesUsed() == 0);
    CHECK(cache.get(bufferKey(clip)) == nullptr);
}

TEST_CASE("StretchCache: missing source not queued", "[stretchcache]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir t;
    SourcePool pool;
    Project p;
    auto clip = addSineSource(p, pool, t.dir, 2.0);

    SECTION("source marked missing")
    {
        p.sources[0].missing = true;
    }
    SECTION("source not loaded in the pool")
    {
        pool.clear();
    }
    SECTION("source not in the project")
    {
        p.sources.clear();
    }

    StretchCache cache;
    cache.require(p, pool);
    CHECK_FALSE(cache.isPending(bufferKey(clip)));
    cache.waitForIdle();
    CHECK(cache.get(bufferKey(clip)) == nullptr);
    CHECK(cache.bytesUsed() == 0);
}

TEST_CASE("StretchCache: rapid changes end in the right buffers", "[stretchcache]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir t;
    SourcePool pool;
    Project p;
    const auto clip = addSineSource(p, pool, t.dir, 1.5);

    StretchCache cache;
    for (double r : { 1.5, 2.0, 1.5, 3.0 })
        cache.require(withStretch(p, r), pool);
    cache.waitForIdle();

    auto c3 = clip;
    c3.stretch = 3.0;
    auto d = cache.get(bufferKey(c3));
    REQUIRE(d != nullptr);
    CHECK(d->getNumSamples() == expectedLen(3.0));
    CHECK_FALSE(cache.isPending(bufferKey(c3)));

    // Whatever else got stored is stored under its own ratio.
    for (double r : { 1.5, 2.0 })
    {
        auto c = clip;
        c.stretch = r;
        CHECK_FALSE(cache.isPending(bufferKey(c)));
        if (auto other = cache.get(bufferKey(c)))
            CHECK(other->getNumSamples() == expectedLen(r));
    }
}

TEST_CASE("StretchCache: keeps unneeded results, evicts least recently used first", "[stretchcache]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir t;
    SourcePool pool;
    Project p;
    auto keep = addSineSource(p, pool, t.dir, 2.0); // track 0: needed throughout, rendered first (oldest)
    Clip other = keep;
    other.id = newId();
    Track t2;
    t2.id = newId();
    t2.clips.push_back(other);
    p.tracks.push_back(t2);

    StretchCache cache(size_t(1) << 20);
    const auto keyFor = [&](double r) {
        auto c = other;
        c.stretch = r;
        return bufferKey(c);
    };
    // 2.0 = 384 KB stays needed; 1.5 (288 KB), 1.25 (240 KB), 1.75 (336 KB) become unneeded in turn.
    for (double r : { 1.5, 1.25, 1.75, 1.0 })
    {
        p.tracks[1].clips[0].stretch = r;
        cache.require(p, pool);
        cache.waitForIdle();
    }

    CHECK(cache.get(bufferKey(keep)) != nullptr);
    CHECK(cache.get(keyFor(1.5)) == nullptr);  // oldest unneeded: evicted
    CHECK(cache.get(keyFor(1.25)) != nullptr); // unneeded but still within the cap
    CHECK(cache.get(keyFor(1.75)) != nullptr);
    CHECK(cache.bytesUsed() <= (size_t(1) << 20));
    CHECK(cache.bytesUsed() == (size_t) (expectedLen(2.0) + expectedLen(1.25) + expectedLen(1.75)) * sizeof(float));
}

TEST_CASE("StretchCache: needed keys are never evicted, even over the cap", "[stretchcache]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir t;
    SourcePool pool;
    Project p;
    const auto clip = addSineSource(p, pool, t.dir, 2.0);

    StretchCache cache(1024);
    cache.require(p, pool);
    cache.waitForIdle();
    CHECK(cache.get(bufferKey(clip)) != nullptr);
    CHECK(cache.bytesUsed() > 1024);
}

TEST_CASE("StretchCache: destroying it with renders in flight is safe", "[stretchcache]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir t;
    SourcePool pool;
    Project p;
    addSineSource(p, pool, t.dir, 4.0, 10 * 48000);
    for (double r : { 3.0, 2.5 }) // three renders: two running, one queued
    {
        auto c = p.tracks[0].clips[0];
        c.id = newId();
        c.stretch = r;
        p.tracks[0].clips.push_back(c);
    }
    int ready = 0;
    {
        auto cache = std::make_unique<StretchCache>();
        cache->onReady = [&] { ++ready; };
        cache->require(p, pool);
        cache.reset(); // cancels and joins the running render
    }
    CHECK(ready == 0);
}

TEST_CASE("countStretching counts clips whose stretched buffer is not ready", "[stretchcache]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir t;
    SourcePool pool;
    Project p;
    const auto clip = addSineSource(p, pool, t.dir, 2.0);
    StretchCache cache;

    CHECK(countStretching(p, pool, cache) == 1); // not rendered yet
    cache.require(p, pool);
    cache.waitForIdle();
    CHECK(countStretching(p, pool, cache) == 0); // ready
    REQUIRE(cache.get(bufferKey(clip)) != nullptr);

    SECTION("a second not-ready clip")
    {
        Clip c2 = clip;
        c2.id = newId();
        c2.stretch = 3.0;
        p.tracks[0].clips.push_back(c2);
        CHECK(countStretching(p, pool, cache) == 1);
    }
    SECTION("missing source is not counted")
    {
        Clip c2 = clip;
        c2.id = newId();
        c2.stretch = 3.0;
        p.tracks[0].clips.push_back(c2);
        p.sources[0].missing = true;
        CHECK(countStretching(p, pool, cache) == 0);
    }
    SECTION("ratio 1 is not counted")
    {
        p.tracks[0].clips[0].stretch = 1.0;
        CHECK(countStretching(p, pool, cache) == 0);
    }
    SECTION("unknown source is not counted")
    {
        Clip c2 = clip;
        c2.stretch = 3.0;
        p.tracks[0].clips.push_back(c2);
        p.sources.clear();
        CHECK(countStretching(p, pool, cache) == 0);
    }
}

TEST_CASE("StretchCache::clear drops entries and ignores late renders", "[stretchcache]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir t;
    SourcePool pool;
    Project p;
    const auto clip = addSineSource(p, pool, t.dir, 2.0);

    StretchCache cache;
    int ready = 0;
    cache.onReady = [&] { ++ready; };
    cache.require(p, pool);
    cache.waitForIdle();
    REQUIRE(cache.get(bufferKey(clip)) != nullptr);
    CHECK(cache.bytesUsed() > 0);

    cache.clear();
    CHECK(cache.get(bufferKey(clip)) == nullptr);
    CHECK(cache.bytesUsed() == 0);
    CHECK_FALSE(cache.isPending(bufferKey(clip)));

    // With a job in flight: clear, and nothing stale is stored afterwards.
    cache.require(p, pool);
    CHECK(cache.isPending(bufferKey(clip)));
    cache.clear();
    CHECK_FALSE(cache.isPending(bufferKey(clip)));
    ready = 0;
    cache.waitForIdle();
    CHECK(cache.get(bufferKey(clip)) == nullptr);
    CHECK(cache.bytesUsed() == 0);
    CHECK(ready == 0);

    // It still works afterwards.
    cache.require(p, pool);
    cache.waitForIdle();
    CHECK(cache.get(bufferKey(clip)) != nullptr);
}

TEST_CASE("StretchCache: a failed render is remembered, not retried and not pending", "[stretchcache]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir t;
    SourcePool pool;
    Project p;
    const auto clip = addSineSource(p, pool, t.dir, 2.0);
    const auto key = bufferKey(clip);

    const bool throws = GENERATE(false, true);
    CAPTURE(throws);
    std::atomic<int> calls { 0 };
    std::atomic<bool> fail { true };
    StretchCache cache(size_t(2) << 30,
                       [&](const juce::AudioBuffer<float>& in, SamplePos, SamplePos, double, double ratio,
                           const std::function<bool(float)>&) -> std::optional<juce::AudioBuffer<float>> {
                           ++calls;
                           if (fail.load())
                           {
                               if (throws)
                                   throw std::bad_alloc();
                               return std::nullopt;
                           }
                           return juce::AudioBuffer<float>(in.getNumChannels(), expectedLen(ratio));
                       });
    int ready = 0;
    cache.onReady = [&] { ++ready; };

    cache.require(p, pool);
    CHECK(cache.isPending(key));
    CHECK(countStretching(p, pool, cache) == 1);
    cache.waitForIdle();

    CHECK(calls == 1);
    CHECK(ready >= 1); // listeners hear about the failure, so they can stop waiting
    CHECK(cache.get(key) == nullptr);
    CHECK(cache.hasFailed(key));
    CHECK(cache.numFailed() == 1);
    CHECK_FALSE(cache.isPending(key));
    CHECK(countStretching(p, pool, cache) == 0); // nothing left to wait for: export/align are not blocked
    CHECK(countStretchFailed(p, pool, cache) == 1);

    // Requiring the same project again does not queue it again.
    for (int i = 0; i < 3; ++i)
    {
        cache.require(p, pool);
        CHECK_FALSE(cache.isPending(key));
        cache.waitForIdle();
    }
    CHECK(calls == 1);
    CHECK(cache.hasFailed(key));

    SECTION("it is retried once the key was no longer needed in between")
    {
        fail = false;
        cache.require(withStretch(p, 1.0), pool);
        CHECK_FALSE(cache.hasFailed(key));
        CHECK(cache.numFailed() == 0);
        cache.require(p, pool);
        CHECK(cache.isPending(key));
        cache.waitForIdle();
        CHECK(calls == 2);
        CHECK(cache.get(key) != nullptr);
        CHECK_FALSE(cache.hasFailed(key));
        CHECK(countStretching(p, pool, cache) == 0);
        CHECK(countStretchFailed(p, pool, cache) == 0);
    }
    SECTION("clear() forgets the failure")
    {
        fail = false;
        cache.clear();
        CHECK_FALSE(cache.hasFailed(key));
        CHECK(countStretching(p, pool, cache) == 1);
        cache.require(p, pool);
        cache.waitForIdle();
        CHECK(calls == 2);
        CHECK(cache.get(key) != nullptr);
    }
    SECTION("other keys still render while one has failed")
    {
        fail = false;
        Clip c2 = clip;
        c2.id = newId();
        c2.stretch = 3.0;
        p.tracks[0].clips.push_back(c2);
        cache.require(p, pool);
        cache.waitForIdle();
        CHECK(cache.get(bufferKey(c2)) != nullptr);
        CHECK(cache.hasFailed(key)); // still needed, still not retried
        CHECK(calls == 2);
        CHECK(countStretching(p, pool, cache) == 0);
        CHECK(countStretchFailed(p, pool, cache) == 1);
    }
}

TEST_CASE("StretchCache: a cancelled render is not a failure", "[stretchcache]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir t;
    SourcePool pool;
    Project p;
    const auto clip = addSineSource(p, pool, t.dir, 2.0, 10 * 48000);

    StretchCache cache;
    cache.require(p, pool);
    cache.require(withStretch(p, 1.0), pool); // cancels it
    cache.waitForIdle();
    CHECK(cache.numFailed() == 0);
    CHECK_FALSE(cache.hasFailed(bufferKey(clip)));
}

// ---------------------------------------------------------------------------------------------------------------------
// Rendering only the part of a source its clips play
// ---------------------------------------------------------------------------------------------------------------------
namespace
{
constexpr SamplePos kLongRate = 8000; // a low project rate keeps two minutes of source small

SamplePos secs(double s) { return (SamplePos) std::llround(s * (double) kLongRate); }

// Makes `c` play the source seconds [from, to) at its ratio.
void setSpan(Clip& c, double from, double to)
{
    c.sourceOffset = (SamplePos) std::llround((double) secs(from) * c.stretch);
    c.length = (SamplePos) std::llround((double) secs(to) * c.stretch) - c.sourceOffset;
}

// Two minutes of sine imported at 8 kHz (the project's rate from here on), with one clip at `stretch` playing the
// source seconds [from, to).
Clip addLongSource(Project& p, SourcePool& pool, const juce::File& dir, double stretch, double from, double to,
                   int seconds = 120)
{
    p.sampleRate = (double) kLongRate;
    const int len = (int) secs(seconds);
    juce::AudioBuffer<float> b(1, len);
    for (int i = 0; i < len; ++i)
        b.setSample(0, i, 0.5f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / (double) kLongRate));
    const auto f = dir.getChildFile(juce::Uuid().toString() + ".wav");
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os(f.createOutputStream());
        REQUIRE(os != nullptr);
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), (double) kLongRate, 1, 32, {}, 0));
        REQUIRE(w != nullptr);
        os.release();
        REQUIRE(w->writeFromAudioSampleBuffer(b, 0, len));
    }
    juce::String err;
    auto info = pool.import(f, (double) kLongRate, err);
    REQUIRE(info.has_value());
    REQUIRE(info->length == len);
    p.sources.push_back(*info);

    Clip c;
    c.id = newId();
    c.sourceId = info->id;
    c.stretch = stretch;
    setSpan(c, from, to);
    Track t;
    t.id = newId();
    t.clips.push_back(c);
    p.tracks.push_back(t);
    return c;
}

// What the fake render puts at index i of a whole-source render.
float wholeValue(SamplePos i) { return 0.001f * (float) (1 + i % 997); }

// A render that takes no time, lays its output out like stretchRange, and can be held back or made to fail.
struct FakeRender
{
    std::atomic<int> calls { 0 };
    std::atomic<bool> hold { false }, fail { false };

    StretchCache::RenderFn fn()
    {
        return [this](const juce::AudioBuffer<float>& in, SamplePos srcBegin, SamplePos srcEnd, double, double ratio,
                      const std::function<bool(float)>& progress) -> std::optional<juce::AudioBuffer<float>> {
            ++calls;
            while (hold.load())
            {
                if (! progress(0.0f))
                    return std::nullopt;
                juce::Thread::sleep(1);
            }
            if (fail.load())
                return std::nullopt;
            const auto start = stretchedIndex(srcBegin, ratio);
            juce::AudioBuffer<float> out(in.getNumChannels(), (int) (stretchedIndex(srcEnd, ratio) - start));
            for (int ch = 0; ch < out.getNumChannels(); ++ch)
                for (int i = 0; i < out.getNumSamples(); ++i)
                    out.setSample(ch, i, wholeValue(start + i));
            return out;
        };
    }
};

juce::Range<SamplePos> secRange(double from, double to) { return { secs(from), secs(to) }; }
} // namespace

TEST_CASE("clipSourceRange and stretchSpanFor", "[stretchcache]")
{
    Clip c;
    c.sourceOffset = 100;
    c.length = 50;
    c.stretch = 2.0;
    CHECK(clipSourceRange(c, 1000) == juce::Range<SamplePos>(50, 75));
    c.stretch = 0.5;
    CHECK(clipSourceRange(c, 1000) == juce::Range<SamplePos>(200, 300));
    CHECK(clipSourceRange(c, 250) == juce::Range<SamplePos>(200, 250)); // clamped to the source
    CHECK(clipSourceRange(c, 150).isEmpty());                           // wholly beyond its end
    c.stretch = 1.5; // 100 / 1.5 = 66.7, 150 / 1.5 = 100: rounded outwards
    CHECK(clipSourceRange(c, 1000) == juce::Range<SamplePos>(66, 100));

    const SamplePos len = secs(120.5);
    const auto rate = (double) kLongRate;
    auto s = stretchSpanFor(secRange(50, 55), len, rate);
    CHECK(s.padded == secRange(49.5, 55.5));
    CHECK(s.render == secRange(40, 60));
    s = stretchSpanFor(secRange(0, 3), len, rate); // no padding before the source's start
    CHECK(s.padded == secRange(0, 3.5));
    CHECK(s.render == secRange(0, 10));
    s = stretchSpanFor(secRange(117, 120.5), len, rate); // nor after its end, which is not a block boundary
    CHECK(s.padded == secRange(116.5, 120.5));
    CHECK(s.render == secRange(110, 120.5));
    s = stretchSpanFor(secRange(9.8, 10.1), len, rate); // padding reaches into the neighbouring blocks
    CHECK(s.render == secRange(0, 20));
    s = stretchSpanFor(secRange(2, 3), secs(7), rate); // a source shorter than a block is rendered whole
    CHECK(s.render == secRange(0, 7));
    s = stretchSpanFor({}, 0, rate);
    CHECK(s.render.isEmpty());
}

TEST_CASE("StretchCache: a clip playing a few seconds of a long source renders only the blocks around them",
          "[stretchcache]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir t;
    SourcePool pool;
    Project p;
    const auto clip = addLongSource(p, pool, t.dir, 2.0, 50, 55);
    const auto key = bufferKey(clip);
    // Two more ratios of the same source, at its start and at its end.
    Clip head = clip, tail = clip;
    head.id = newId();
    head.stretch = 1.5;
    setSpan(head, 0, 3);
    tail.id = newId();
    tail.stretch = 0.5;
    setSpan(tail, 117, 120);
    p.tracks[0].clips.push_back(head);
    p.tracks[0].clips.push_back(tail);

    FakeRender fake;
    StretchCache cache(size_t(2) << 30, fake.fn());
    CHECK_FALSE(cache.covers(clip));
    cache.require(p, pool);
    cache.waitForIdle();
    CHECK(fake.calls == 3);

    // 5 s of 120 s: the 20 s of source in the two blocks it touches (with padding), not the whole source.
    const auto got = cache.find(key);
    REQUIRE(got.data != nullptr);
    CHECK(cache.sourceRange(key) == secRange(40, 60));
    CHECK(got.start == secs(40) * 2);
    CHECK(got.data->getNumSamples() == secs(20) * 2);
    CHECK(got.data->getNumSamples() * 5 < expectedLen(2.0, (int) secs(120)));
    CHECK(got.data->getSample(0, 0) == wholeValue(got.start));
    CHECK(cache.covers(clip));

    const auto h = cache.find(bufferKey(head));
    REQUIRE(h.data != nullptr);
    CHECK(cache.sourceRange(bufferKey(head)) == secRange(0, 10));
    CHECK(h.start == 0);
    CHECK(h.data->getNumSamples() == secs(10) * 3 / 2);
    const auto tl = cache.find(bufferKey(tail));
    REQUIRE(tl.data != nullptr);
    CHECK(cache.sourceRange(bufferKey(tail)) == secRange(110, 120));
    CHECK(tl.start == secs(110) / 2);
    CHECK(tl.data->getNumSamples() == secs(10) / 2);

    CHECK(cache.bytesUsed() == (size_t) (secs(20) * 2 + secs(10) * 3 / 2 + secs(10) / 2) * sizeof(float));
    CHECK(countStretching(p, pool, cache) == 0);
    CHECK(countStretchFailed(p, pool, cache) == 0);
}

TEST_CASE("StretchCache: the real render of a part is far smaller than the whole stretched source", "[stretchcache]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir t;
    SourcePool pool;
    Project p;
    const auto clip = addLongSource(p, pool, t.dir, 1.25, 50, 55);

    StretchCache cache; // Rubber Band
    cache.require(p, pool);
    cache.waitForIdle();
    const auto got = cache.find(bufferKey(clip));
    REQUIRE(got.data != nullptr);
    CHECK(got.start == secs(40) * 5 / 4);
    CHECK(got.data->getNumSamples() == secs(20) * 5 / 4);
    CHECK(cache.bytesUsed() == (size_t) (secs(20) * 5 / 4) * sizeof(float));
    CHECK(cache.bytesUsed() * 5 < (size_t) expectedLen(1.25, (int) secs(120)) * sizeof(float));
    // There is audio where the clip plays.
    const int first = (int) (clip.sourceOffset - got.start);
    CHECK(got.data->getMagnitude(0, first, (int) clip.length) > 0.2f);
}

TEST_CASE("StretchCache: clips of one key share one buffer that covers all of them", "[stretchcache]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir t;
    SourcePool pool;
    Project p;
    const auto a = addLongSource(p, pool, t.dir, 2.0, 12, 14);
    Clip b = a, other = a;
    b.id = newId();
    setSpan(b, 47, 49);
    other.id = newId();
    other.stretch = 3.0;
    setSpan(other, 100, 101);
    p.tracks[0].clips.push_back(b);
    p.tracks[0].clips.push_back(other);
    REQUIRE(bufferKey(a) == bufferKey(b));

    FakeRender fake;
    StretchCache cache(size_t(2) << 30, fake.fn());
    cache.require(p, pool);
    cache.waitForIdle();

    CHECK(fake.calls == 2); // one per key, not one per clip
    CHECK(cache.sourceRange(bufferKey(a)) == secRange(10, 50)); // the hull of both clips, in whole blocks
    CHECK(cache.covers(a));
    CHECK(cache.covers(b));
    CHECK(cache.sourceRange(bufferKey(other)) == secRange(90, 110));
    CHECK(cache.covers(other));

    // Removing one clip keeps the larger buffer: nothing is rendered again.
    p.tracks[0].clips.erase(p.tracks[0].clips.begin() + 1);
    cache.require(p, pool);
    CHECK_FALSE(cache.isPending(bufferKey(a)));
    cache.waitForIdle();
    CHECK(fake.calls == 2);
    CHECK(cache.sourceRange(bufferKey(a)) == secRange(10, 50));
}

TEST_CASE("StretchCache: trims inside the rendered blocks render nothing; growing beyond them renders again",
          "[stretchcache]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir t;
    SourcePool pool;
    Project p;
    const auto clip = addLongSource(p, pool, t.dir, 2.0, 50, 55);
    const auto key = bufferKey(clip);
    auto& c = p.tracks[0].clips[0];

    FakeRender fake;
    StretchCache cache(size_t(2) << 30, fake.fn());
    int ready = 0;
    cache.onReady = [&] { ++ready; };
    cache.require(p, pool);
    cache.waitForIdle();
    REQUIRE(cache.sourceRange(key) == secRange(40, 60));
    const auto first = cache.get(key);

    // Trims and edge drags that stay (with their padding) inside [40 s, 60 s).
    for (auto [from, to] : { std::pair { 51.0, 54.0 }, std::pair { 45.0, 58.0 }, std::pair { 40.5, 59.5 },
                             std::pair { 50.0, 55.0 } })
    {
        CAPTURE(from, to);
        setSpan(c, from, to);
        cache.require(p, pool);
        CHECK_FALSE(cache.isPending(key));
        CHECK(cache.covers(c));
        CHECK(countStretching(p, pool, cache) == 0);
    }
    cache.waitForIdle();
    CHECK(fake.calls == 1);
    CHECK(cache.get(key) == first);

    // Growing past the block (the padding would end at 60.3 s): a larger render is queued, and until it lands the
    // old buffer is still what get() returns.
    fake.hold = true;
    ready = 0;
    setSpan(c, 50, 59.8);
    cache.require(p, pool);
    CHECK(cache.isPending(key));
    CHECK(cache.get(key) == first);
    CHECK(cache.sourceRange(key) == secRange(40, 60));
    CHECK(cache.covers(c)); // the clip itself still fits; only its padding does not
    cache.require(p, pool);  // asking again does not queue a second job
    setSpan(c, 50, 63);
    cache.require(p, pool); // still within the job on its way ([40 s, 70 s))
    CHECK(cache.isPending(key));
    CHECK(cache.get(key) == first);
    CHECK_FALSE(cache.covers(c));
    CHECK(countStretching(p, pool, cache) == 1); // part of the clip is not there yet
    CHECK(countStretchFailed(p, pool, cache) == 0);
    CHECK(ready == 0);

    fake.hold = false;
    cache.waitForIdle();
    CHECK(fake.calls == 2);
    CHECK(ready >= 1);
    CHECK_FALSE(cache.isPending(key));
    const auto second = cache.find(key);
    REQUIRE(second.data != nullptr);
    CHECK(second.data != first);
    CHECK(cache.sourceRange(key) == secRange(40, 70));
    CHECK(second.start == secs(40) * 2);
    CHECK(second.data->getNumSamples() == secs(30) * 2);
    CHECK(cache.covers(c));
    CHECK(countStretching(p, pool, cache) == 0);
    CHECK(cache.bytesUsed() == (size_t) (secs(30) * 2) * sizeof(float)); // the old buffer was replaced, not kept
    CHECK(first->getNumSamples() == secs(20) * 2); // whoever still holds the old one keeps a valid buffer

    // Growing and shrinking back before the render lands: the job is dropped, the buffer in place stays.
    fake.hold = true;
    setSpan(c, 30, 55);
    cache.require(p, pool);
    CHECK(cache.isPending(key));
    setSpan(c, 50, 55);
    cache.require(p, pool);
    CHECK_FALSE(cache.isPending(key));
    fake.hold = false;
    cache.waitForIdle();
    CHECK(cache.get(key) == second.data);
    CHECK(cache.sourceRange(key) == secRange(40, 70));
    CHECK(cache.numFailed() == 0);
}

TEST_CASE("StretchCache: a buffer larger than needed is kept unless the cache is over its cap and it is far larger",
          "[stretchcache]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir t;
    SourcePool pool;
    Project p;
    const auto clip = addLongSource(p, pool, t.dir, 2.0, 10, 100);
    const auto key = bufferKey(clip);
    auto& c = p.tracks[0].clips[0];
    FakeRender fake;

    SECTION("under the cap it is always kept")
    {
        StretchCache cache(size_t(2) << 30, fake.fn());
        cache.require(p, pool);
        cache.waitForIdle();
        REQUIRE(cache.sourceRange(key) == secRange(0, 110));
        setSpan(c, 50, 55);
        cache.require(p, pool);
        CHECK_FALSE(cache.isPending(key));
        cache.waitForIdle();
        CHECK(fake.calls == 1);
        CHECK(cache.sourceRange(key) == secRange(0, 110));
    }
    SECTION("over the cap it is rendered smaller once it is more than twice what is needed")
    {
        StretchCache cache(1024, fake.fn());
        cache.require(p, pool);
        cache.waitForIdle();
        REQUIRE(cache.sourceRange(key) == secRange(0, 110));
        const auto big = cache.bytesUsed();

        setSpan(c, 10, 60); // needs [0 s, 70 s): 110 s is not twice that
        cache.require(p, pool);
        CHECK_FALSE(cache.isPending(key));
        CHECK(fake.calls == 1);

        fake.hold = true;
        setSpan(c, 50, 55); // needs [40 s, 60 s)
        cache.require(p, pool);
        CHECK(cache.isPending(key));
        CHECK(cache.sourceRange(key) == secRange(0, 110)); // served until the smaller one lands
        CHECK(countStretching(p, pool, cache) == 0);       // and it holds all the clip plays
        fake.hold = false;
        cache.waitForIdle();
        CHECK(fake.calls == 2);
        CHECK(cache.sourceRange(key) == secRange(40, 60));
        CHECK(cache.bytesUsed() < big / 5);
        CHECK(cache.bytesUsed() > 1024); // still over the cap: what a clip plays is never dropped

        cache.require(p, pool); // and that is stable
        CHECK_FALSE(cache.isPending(key));
        cache.waitForIdle();
        CHECK(fake.calls == 2);
    }
}

TEST_CASE("StretchCache: when a larger render fails the old buffer stays and the clip counts as failed",
          "[stretchcache]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempDir t;
    SourcePool pool;
    Project p;
    const auto clip = addLongSource(p, pool, t.dir, 2.0, 50, 55);
    const auto key = bufferKey(clip);
    auto& c = p.tracks[0].clips[0];

    FakeRender fake;
    StretchCache cache(size_t(2) << 30, fake.fn());
    cache.require(p, pool);
    cache.waitForIdle();
    const auto first = cache.get(key);
    REQUIRE(first != nullptr);

    fake.fail = true;
    setSpan(c, 50, 75);
    cache.require(p, pool);
    cache.waitForIdle();
    CHECK(fake.calls == 2);
    CHECK(cache.hasFailed(key));
    CHECK_FALSE(cache.isPending(key));
    CHECK(cache.get(key) == first); // what was rendered still plays
    CHECK_FALSE(cache.covers(c));
    CHECK(countStretching(p, pool, cache) == 0);
    CHECK(countStretchFailed(p, pool, cache) == 1);

    cache.require(p, pool); // not retried while it stays needed
    CHECK_FALSE(cache.isPending(key));
    cache.waitForIdle();
    CHECK(fake.calls == 2);

    // Back inside the buffer that is there: the clip is whole again.
    setSpan(c, 50, 55);
    cache.require(p, pool);
    CHECK_FALSE(cache.isPending(key));
    CHECK(countStretchFailed(p, pool, cache) == 0);
    CHECK(countStretching(p, pool, cache) == 0);
}
