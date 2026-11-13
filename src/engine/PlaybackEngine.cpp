#include "engine/PlaybackEngine.h"

namespace aligna
{
PlaybackEngine::~PlaybackEngine()
{
    current.store(nullptr);
    retired.clear();
    latest.reset();
}

void PlaybackEngine::setSnapshot(std::shared_ptr<const MixSnapshot> s)
{
    const auto now = juce::Time::getMillisecondCounter();
    if (latest)
        retired.push_back({ std::move(latest), now });
    latest = std::move(s);
    current.store(latest.get(), std::memory_order_release);

    std::erase_if(retired, [now](const Retired& r) { return (juce::uint32) (now - r.retiredAtMs) >= 500u; });
}

void PlaybackEngine::play()
{
    playStart = position.load();
    playing.store(true);
}

void PlaybackEngine::pause() { playing.store(false); }

void PlaybackEngine::stop()
{
    playing.store(false);
    position.store(playStart);
}

void PlaybackEngine::setPosition(SamplePos p) { position.store(std::max<SamplePos>(0, p)); }

void PlaybackEngine::getNextAudioBlock(const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();
    // Load the position before the playing flag: a message-thread pause() followed by setPosition()
    // then either leaves playing false here, or makes the compare-exchange below fail.
    auto pos = position.load();
    if (!playing.load())
        return;
    const auto* snap = current.load(std::memory_order_acquire);
    if (snap != nullptr)
        renderMix(*snap, pos, *info.buffer, info.startSample, info.numSamples);
    // An external stop()/setPosition() during the render wins over the advance.
    position.compare_exchange_strong(pos, pos + info.numSamples);
}
} // namespace aligna
