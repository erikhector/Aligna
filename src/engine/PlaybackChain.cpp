#include "engine/PlaybackChain.h"
#include <cmath>

namespace aligna
{
PlaybackChain::PlaybackChain(PlaybackEngine& e, double rate) : resampler(&e, false, 2), projectRate(rate)
{
    player.setSource(&resampler);
}

PlaybackChain::~PlaybackChain() { player.setSource(nullptr); }

void PlaybackChain::setProjectRate(double rate, juce::AudioDeviceManager& dm)
{
    if (std::abs(rate - projectRate.load()) < 1e-9)
        return;
    projectRate.store(rate);
    dm.removeAudioCallback(this); // audioDeviceStopped
    dm.addAudioCallback(this);    // audioDeviceAboutToStart(current device), if any
}

void PlaybackChain::audioDeviceAboutToStart(juce::AudioIODevice* d)
{
    const auto deviceRate = d != nullptr ? d->getCurrentSampleRate() : 0.0;
    if (deviceRate > 0.0)
    {
        const auto ratio = projectRate.load() / deviceRate;
        resampler.setResamplingRatio(ratio);
        appliedRatio.store(ratio);
    }
    player.audioDeviceAboutToStart(d);
}

void PlaybackChain::audioDeviceStopped() { player.audioDeviceStopped(); }

void PlaybackChain::audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out,
                                                     int numOut, int n,
                                                     const juce::AudioIODeviceCallbackContext& ctx)
{
    player.audioDeviceIOCallbackWithContext(in, numIn, out, numOut, n, ctx);
}
} // namespace aligna
