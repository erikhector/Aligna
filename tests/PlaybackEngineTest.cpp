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

TEST_CASE("PlaybackEngine frees a retired snapshot at once when no block is in flight", "[engine]")
{
    PlaybackEngine e;
    auto s1 = makeSnap(0.2f);
    std::weak_ptr<const MixSnapshot> w = s1;
    e.setSnapshot(std::move(s1));
    REQUIRE_FALSE(w.expired()); // current
    e.play();
    juce::AudioBuffer<float> a(2, 64);
    pull(e, a); // a whole block, begun and ended: nothing is in flight afterwards
    e.setSnapshot(makeSnap(0.3f));
    REQUIRE(w.expired());
    REQUIRE_FALSE(e.purgeRetired());
}

TEST_CASE("PlaybackEngine keeps a snapshot retired during a block until that block ends", "[engine]")
{
    PlaybackEngine e;
    REQUIRE_FALSE(e.purgeRetired()); // nothing retired
    auto s1 = makeSnap(0.2f);
    const auto* raw1 = s1.get();
    std::weak_ptr<const MixSnapshot> w = s1;
    e.setSnapshot(std::move(s1));
    REQUIRE_FALSE(e.purgeRetired()); // the current snapshot is not retired
    e.play();

    PlaybackEngine::Block b;
    REQUIRE(e.beginBlock(b)); // the audio thread is now rendering from s1
    REQUIRE(b.snapshot == raw1);
    e.setSnapshot(std::make_shared<const MixSnapshot>());
    REQUIRE_FALSE(w.expired());
    REQUIRE(e.purgeRetired()); // still in use, however long ago it was replaced
    REQUIRE(e.purgeRetired());
    REQUIRE_FALSE(w.expired());
    juce::AudioBuffer<float> out(2, 64);
    out.clear();
    renderMix(*b.snapshot, b.position, out, 0, 64); // still valid memory
    REQUIRE(out.getMagnitude(0, 64) > 0.0f);

    e.endBlock(b, 64);
    REQUIRE_FALSE(w.expired());      // freeing is the message thread's job
    REQUIRE_FALSE(e.purgeRetired()); // freed without a further setSnapshot
    REQUIRE(w.expired());
}

TEST_CASE("PlaybackEngine block in flight only protects the snapshots it can hold", "[engine]")
{
    PlaybackEngine e;
    auto s1 = makeSnap(0.1f), s2 = makeSnap(0.2f), s3 = makeSnap(0.3f);
    std::weak_ptr<const MixSnapshot> w1 = s1, w2 = s2, w3 = s3;
    const auto* raw3 = s3.get();
    e.setSnapshot(std::move(s1));
    e.play();

    PlaybackEngine::Block a;
    REQUIRE(e.beginBlock(a)); // holds s1
    e.setSnapshot(std::move(s2));
    e.setSnapshot(std::move(s3)); // s2 was current during the block, which may have loaded it: kept as well
    REQUIRE_FALSE(w1.expired());
    REQUIRE_FALSE(w2.expired());
    e.endBlock(a, 64);

    PlaybackEngine::Block b;
    REQUIRE(e.beginBlock(b)); // a later block: holds s3, and keeps neither s1 nor s2 alive
    REQUIRE(b.snapshot == raw3);
    REQUIRE_FALSE(e.purgeRetired());
    REQUIRE(w1.expired());
    REQUIRE(w2.expired());
    e.setSnapshot(nullptr); // s3 retired while b renders from it
    REQUIRE_FALSE(w3.expired());
    REQUIRE(e.purgeRetired());
    e.endBlock(b, 64);
    REQUIRE_FALSE(e.purgeRetired());
    REQUIRE(w3.expired());
}

TEST_CASE("PlaybackEngine purgeRetired(waitForAll) does not wait when no block is in flight", "[engine]")
{
    PlaybackEngine e;
    auto s1 = makeSnap(0.2f);
    std::weak_ptr<const MixSnapshot> w = s1;
    e.setSnapshot(std::move(s1));
    e.setSnapshot(nullptr);
    const auto t0 = juce::Time::getMillisecondCounter();
    REQUIRE_FALSE(e.purgeRetired(true));
    REQUIRE(juce::Time::getMillisecondCounter() - t0 < 400);
    REQUIRE(w.expired());
    e.play(); // no snapshot: silent
    juce::AudioBuffer<float> a(2, 64);
    pull(e, a);
    REQUIRE_THAT(a.getMagnitude(0, 64), WithinAbs(0.0, 1e-9));
}

TEST_CASE("PlaybackEngine purgeRetired(waitForAll) waits for the block in flight to end", "[engine]")
{
    PlaybackEngine e;
    auto s1 = makeSnap(0.2f);
    std::weak_ptr<const MixSnapshot> w = s1;
    e.setSnapshot(std::move(s1));
    e.play();
    PlaybackEngine::Block b;
    REQUIRE(e.beginBlock(b));
    e.setSnapshot(nullptr);
    std::atomic<bool> ended { false };
    std::thread audio([&] {
        juce::Thread::sleep(60);
        ended = true;
        e.endBlock(b, 64);
    });
    const bool left = e.purgeRetired(true, 20000);
    const bool endedFirst = ended.load();
    audio.join();
    REQUIRE_FALSE(left);
    REQUIRE(endedFirst); // not freed while the block was running
    REQUIRE(w.expired());
}

TEST_CASE("PlaybackEngine purgeRetired(waitForAll) gives up on a block that never ends", "[engine]")
{
    PlaybackEngine e;
    auto s1 = makeSnap(0.2f);
    std::weak_ptr<const MixSnapshot> w = s1;
    e.setSnapshot(std::move(s1));
    e.play();
    PlaybackEngine::Block b;
    REQUIRE(e.beginBlock(b)); // a stalled audio thread: the block stays in flight
    e.setSnapshot(nullptr);

    const auto t0 = juce::Time::getMillisecondCounter();
    REQUIRE(e.purgeRetired(true, 150)); // neither hangs ...
    const auto waited = juce::Time::getMillisecondCounter() - t0;
    REQUIRE(waited >= 150 - 20);
    REQUIRE(waited < 5000);
    REQUIRE_FALSE(w.expired()); // ... nor frees what the block is using
    REQUIRE(b.snapshot->project.tracks.size() == 1);

    e.endBlock(b, 64); // the audio thread resumes
    REQUIRE_FALSE(e.purgeRetired());
    REQUIRE(w.expired());
}

TEST_CASE("PlaybackEngine seek to the position of the block in flight is still a seek", "[engine]")
{
    PlaybackEngine e;
    auto snap = makeSnap(0.5f);
    e.setSnapshot(snap);
    e.setPosition(1000);
    e.play();
    PlaybackEngine::Block b;
    REQUIRE(e.beginBlock(b));
    REQUIRE(b.position == 1000);

    SECTION("prepareStart: recording started during playback, pre-roll off")
    {
        e.prepareStart(1000); // the position the audio thread is rendering right now
        e.endBlock(b, 512);
        REQUIRE(e.getPosition() == 1000); // not advanced to 1512
        REQUIRE_FALSE(e.isPlaying());

        REQUIRE(e.startFromAudioThread());
        juce::AudioBuffer<float> a(2, 512), expect(2, 512);
        pull(e, a);
        expect.clear();
        renderMix(*snap, 1000, expect, 0, 512);
        for (int i = 0; i < 512; ++i)
            REQUIRE_THAT(a.getSample(0, i), WithinAbs(expect.getSample(0, i), 1e-6));
        REQUIRE(e.getPosition() == 1512);
        e.stop();
        REQUIRE(e.getPosition() == 1000);
    }
    SECTION("setPosition")
    {
        e.setPosition(1000);
        e.endBlock(b, 512);
        REQUIRE(e.getPosition() == 1000);
        REQUIRE(e.isPlaying());
    }
    SECTION("stop back to the play start")
    {
        e.stop();
        e.endBlock(b, 512);
        REQUIRE(e.getPosition() == 1000);
        REQUIRE_FALSE(e.isPlaying());
    }
    SECTION("pause is not a seek: the block that was heard still advances")
    {
        e.pause();
        e.endBlock(b, 512);
        REQUIRE(e.getPosition() == 1512);
        REQUIRE_FALSE(e.isPlaying());
    }
    SECTION("no command: the block advances")
    {
        e.endBlock(b, 512);
        REQUIRE(e.getPosition() == 1512);
        REQUIRE(e.isPlaying());
    }
}

TEST_CASE("PlaybackEngine transport command after prepareStart withdraws the pending start", "[engine]")
{
    PlaybackEngine e;
    e.setPosition(50);
    e.play();
    e.prepareStart(1000);

    SECTION("stop")
    {
        e.stop();
        REQUIRE_FALSE(e.startFromAudioThread());
        REQUIRE_FALSE(e.isPlaying());
        REQUIRE(e.getPosition() == 1000);
    }
    SECTION("pause")
    {
        e.pause();
        REQUIRE_FALSE(e.startFromAudioThread());
        REQUIRE_FALSE(e.isPlaying());
    }
    SECTION("setPosition")
    {
        e.setPosition(2000);
        REQUIRE_FALSE(e.startFromAudioThread());
        REQUIRE_FALSE(e.isPlaying());
        REQUIRE(e.getPosition() == 2000);
    }
    SECTION("cancelPendingStart")
    {
        e.cancelPendingStart();
        REQUIRE_FALSE(e.startFromAudioThread());
        REQUIRE_FALSE(e.isPlaying());
        REQUIRE(e.getPosition() == 1000);
    }
    SECTION("a new prepareStart is a new pending start")
    {
        e.stop();
        e.prepareStart(3000);
        REQUIRE(e.startFromAudioThread());
        REQUIRE(e.isPlaying());
        REQUIRE(e.getPosition() == 3000);
    }
    SECTION("a start is consumed once")
    {
        REQUIRE(e.startFromAudioThread());
        e.stop();
        REQUIRE_FALSE(e.startFromAudioThread());
        REQUIRE_FALSE(e.isPlaying());
    }
    SECTION("without prepareStart there is nothing to start")
    {
        e.pause();
        e.setPosition(10);
        REQUIRE_FALSE(e.startFromAudioThread());
        REQUIRE_FALSE(e.isPlaying());
    }
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

TEST_CASE("prepareStart then startFromAudioThread plays from p; stop returns to p", "[engine]")
{
    PlaybackEngine e;
    auto snap = makeSnap(0.5f);
    e.setSnapshot(snap);
    e.setPosition(50);
    e.play();
    e.prepareStart(1000);
    REQUIRE_FALSE(e.isPlaying());
    REQUIRE(e.getPosition() == 1000);

    juce::AudioBuffer<float> a(2, 512), expect(2, 512);
    pull(e, a); // not started yet: silent, no advance
    REQUIRE(e.getPosition() == 1000);
    REQUIRE_THAT(a.getMagnitude(0, 512), WithinAbs(0.0, 1e-9));

    e.startFromAudioThread();
    REQUIRE(e.isPlaying());
    pull(e, a);
    REQUIRE(e.getPosition() == 1512);
    expect.clear();
    renderMix(*snap, 1000, expect, 0, 512);
    for (int i = 0; i < 512; ++i)
        REQUIRE_THAT(a.getSample(0, i), WithinAbs(expect.getSample(0, i), 1e-6));

    e.stop();
    REQUIRE_FALSE(e.isPlaying());
    REQUIRE(e.getPosition() == 1000);
}
