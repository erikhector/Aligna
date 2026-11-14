#include "engine/StretchCache.h"
#include <catch2/catch_test_macros.hpp>
#include <juce_events/juce_events.h>
#include <cmath>

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
