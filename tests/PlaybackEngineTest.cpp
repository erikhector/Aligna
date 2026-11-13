#include "engine/PlaybackEngine.h"
#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace aligna;
using Catch::Matchers::WithinAbs;

namespace
{
std::shared_ptr<const MixSnapshot> makeSnap(float v, int n = 4096)
{
    auto s = std::make_shared<MixSnapshot>();
    auto b = std::make_shared<juce::AudioBuffer<float>>(2, n);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
            b->setSample(c, i, v * (float) (i + 1) / (float) n);
    SourceInfo si;
    si.id = "s";
    si.length = n;
    s->project.sources.push_back(si);
    s->audio["s"] = b;
    Track t;
    t.id = "t";
    Clip c;
    c.id = "c";
    c.sourceId = "s";
    c.length = n;
    t.clips.push_back(c);
    s->project.tracks.push_back(t);
    return s;
}

void pull(PlaybackEngine& e, juce::AudioBuffer<float>& buf)
{
    juce::AudioSourceChannelInfo info(&buf, 0, buf.getNumSamples());
    e.getNextAudioBlock(info);
}
} // namespace

TEST_CASE("PlaybackEngine plays and advances", "[engine]")
{
    PlaybackEngine e;
    auto snap = makeSnap(0.5f);
    e.setSnapshot(snap);
    e.prepareToPlay(512, 48000);
    e.play();
    juce::AudioBuffer<float> a(2, 512), b(2, 512), expect(2, 1024);
    pull(e, a);
    pull(e, b);
    REQUIRE(e.getPosition() == 1024);
    expect.clear();
    renderMix(*snap, 0, expect, 0, 1024);
    for (int i = 0; i < 512; ++i)
    {
        REQUIRE_THAT(a.getSample(0, i), WithinAbs(expect.getSample(0, i), 1e-6));
        REQUIRE_THAT(b.getSample(1, i), WithinAbs(expect.getSample(1, 512 + i), 1e-6));
    }
    REQUIRE(a.getMagnitude(0, 512) > 0.0f);
}

TEST_CASE("PlaybackEngine paused is silent", "[engine]")
{
    PlaybackEngine e;
    e.setSnapshot(makeSnap(0.5f));
    juce::AudioBuffer<float> a(2, 256);
    a.setSize(2, 256);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 256; ++i)
            a.setSample(c, i, 1.0f);
    e.setPosition(100);
    pull(e, a);
    REQUIRE_THAT(a.getMagnitude(0, 256), WithinAbs(0.0, 1e-9));
    REQUIRE(e.getPosition() == 100);
    REQUIRE_FALSE(e.isPlaying());
}

TEST_CASE("PlaybackEngine no snapshot is silent", "[engine]")
{
    PlaybackEngine e;
    e.play();
    juce::AudioBuffer<float> a(2, 128);
    a.clear();
    a.setSample(0, 0, 1.0f);
    pull(e, a);
    REQUIRE_THAT(a.getMagnitude(0, 128), WithinAbs(0.0, 1e-9));
}

TEST_CASE("PlaybackEngine stop returns to play start", "[engine]")
{
    PlaybackEngine e;
    e.setSnapshot(makeSnap(0.5f));
    e.setPosition(300);
    e.play();
    juce::AudioBuffer<float> a(2, 512);
    pull(e, a);
    REQUIRE(e.getPosition() == 812);
    e.stop();
    REQUIRE_FALSE(e.isPlaying());
    REQUIRE(e.getPosition() == 300);
    // pause keeps position
    e.play();
    pull(e, a);
    e.pause();
    REQUIRE(e.getPosition() == 812);
}

TEST_CASE("PlaybackEngine snapshot swap applies at next block", "[engine]")
{
    PlaybackEngine e;
    e.setSnapshot(makeSnap(0.2f));
    e.play();
    juce::AudioBuffer<float> a(2, 64), b(2, 64);
    pull(e, a);
    auto s2 = makeSnap(0.8f);
    e.setSnapshot(s2);
    pull(e, b);
    juce::AudioBuffer<float> expect(2, 64);
    expect.clear();
    renderMix(*s2, 64, expect, 0, 64);
    for (int i = 0; i < 64; ++i)
        REQUIRE_THAT(b.getSample(0, i), WithinAbs(expect.getSample(0, i), 1e-6));
}

TEST_CASE("PlaybackEngine frees retired snapshots", "[engine]")
{
    PlaybackEngine e;
    auto s1 = makeSnap(0.2f);
    std::weak_ptr<const MixSnapshot> w = s1;
    e.setSnapshot(std::move(s1));
    e.setSnapshot(makeSnap(0.3f));
    REQUIRE_FALSE(w.expired()); // retired, still held (<500 ms)
    juce::Thread::sleep(600);
    e.setSnapshot(makeSnap(0.4f));
    REQUIRE(w.expired());
}

TEST_CASE("PlaybackEngine seek during audio block is not lost", "[engine]")
{
    PlaybackEngine e;
    e.setSnapshot(makeSnap(0.5f, 1 << 20));
    juce::AudioBuffer<float> buf(2, 8192);
    std::atomic<bool> run { true };
    std::thread audio([&] {
        while (run.load())
            pull(e, buf);
    });
    int lost = 0;
    for (int i = 0; i < 300; ++i)
    {
        e.play();
        juce::Thread::sleep(1);
        e.pause();
        e.setPosition(12345 + i);
        juce::Thread::sleep(2); // let any in-flight block finish
        if (e.getPosition() != 12345 + i)
            ++lost;
    }
    run = false;
    audio.join();
    REQUIRE(lost == 0);
}
