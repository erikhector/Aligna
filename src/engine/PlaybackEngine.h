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

    // Message thread. The previous snapshot is retired and freed by a later setSnapshot call (>= 500 ms old)
    // or by the destructor, so the audio thread never owns or destroys a snapshot.
    void setSnapshot(std::shared_ptr<const MixSnapshot>);

    void play();
    void pause();
    void stop(); // pause + return to the position where play() was last called
    void setPosition(SamplePos);
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
