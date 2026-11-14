#pragma once
#include "engine/Mixer.h"
#include <atomic>
#include <memory>
#include <vector>

namespace aligna
{
// Real-time transport + mix source. Control methods are message-thread; getNextAudioBlock is audio-thread.
class PlaybackEngine : public juce::AudioSource
{
public:
    PlaybackEngine() = default;
    ~PlaybackEngine() override;

    // A retired snapshot is kept at least this long, so a device callback that was still rendering from it
    // when it was replaced has finished before it is freed.
    static constexpr juce::uint32 retireMs = 500;

    // Message thread. The previous snapshot is retired and freed (once >= retireMs old) by a later setSnapshot
    // or purgeRetired call, or by the destructor, so the audio thread never owns or destroys a snapshot.
    void setSnapshot(std::shared_ptr<const MixSnapshot>);
    // Message thread. Frees the retired snapshots that are >= retireMs old; true if younger ones remain.
    // With waitForAll, first sleeps (at most retireMs) until every retired snapshot is old enough.
    bool purgeRetired(bool waitForAll = false);

    void play();
    void pause();
    void stop(); // pause + return to the position where play() was last called
    void setPosition(SamplePos);
    // Message thread: pause, then position = play start = p. Pair with startFromAudioThread() so playback of p
    // begins in a known device callback.
    void prepareStart(SamplePos p);
    // Audio thread: start playing from the current position (no other state touched).
    void startFromAudioThread() noexcept { playing.store(true); }
    SamplePos getPosition() const { return position.load(); }
    bool isPlaying() const { return playing.load(); }

    void prepareToPlay(int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo&) override;

private:
    struct Retired
    {
        std::shared_ptr<const MixSnapshot> snap;
        juce::uint32 retiredAtMs;
    };

    std::atomic<const MixSnapshot*> current { nullptr };
    std::atomic<SamplePos> position { 0 };
    std::atomic<bool> playing { false };
    SamplePos playStart = 0; // message thread only
    std::shared_ptr<const MixSnapshot> latest;
    std::vector<Retired> retired;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlaybackEngine)
};
} // namespace aligna
