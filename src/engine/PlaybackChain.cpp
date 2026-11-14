#include "engine/PlaybackChain.h"
#include <cmath>

namespace aligna
{
namespace
{
// 1.0 at sample 0, silence afterwards.
struct ImpulseSource : juce::AudioSource
{
    void prepareToPlay(int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();
        if (! fired && info.numSamples > 0)
        {
            for (int c = 0; c < info.buffer->getNumChannels(); ++c)
                info.buffer->setSample(c, info.startSample, 1.0f);
            fired = true;
        }
    }
    bool fired = false;
};
} // namespace

PlaybackChain::PlaybackChain(PlaybackEngine& e, double rate) : engine(e), resampler(&e, false, 2), projectRate(rate)
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

double PlaybackChain::measureResamplerDelay(double ratio, int blockSize)
{
    blockSize = std::max(1, blockSize);
    ImpulseSource src;
    juce::ResamplingAudioSource rs(&src, false, 2);
    rs.setResamplingRatio(ratio);
    rs.prepareToPlay(blockSize, 48000.0);

    const int total = std::max(1, (512 + blockSize - 1) / blockSize) * blockSize;
    juce::AudioBuffer<float> out(2, total);
    for (int pos = 0; pos < total; pos += blockSize)
    {
        juce::AudioSourceChannelInfo info(&out, pos, blockSize);
        rs.getNextAudioBlock(info);
    }
    rs.releaseResources();

    const float* y = out.getReadPointer(0);
    int peak = 0;
    for (int i = 1; i < total; ++i)
        if (std::abs(y[i]) > std::abs(y[peak]))
            peak = i;
    // Parabolic refinement; the signal before the first output sample is silence.
    const double ym = peak > 0 ? y[peak - 1] : 0.0, y0 = y[peak], yp = peak + 1 < total ? y[peak + 1] : 0.0;
    const double denom = ym - 2.0 * y0 + yp;
    const double delta = std::abs(denom) > 1e-12 ? 0.5 * (ym - yp) / denom : 0.0;
    return (double) peak + delta;
}

void PlaybackChain::audioDeviceAboutToStart(juce::AudioIODevice* d)
{
    const auto rate = d != nullptr ? d->getCurrentSampleRate() : 0.0;
    double ratio = appliedRatio.load();
    if (rate > 0.0)
    {
        ratio = projectRate.load() / rate;
        resampler.setResamplingRatio(ratio);
        appliedRatio.store(ratio);
    }
    deviceRate.store(rate);
    inputLatency.store(d != nullptr ? d->getInputLatencyInSamples() : 0);
    outputLatency.store(d != nullptr ? d->getOutputLatencyInSamples() : 0);
    chainDelay.store(measureResamplerDelay(ratio, d != nullptr ? d->getCurrentBufferSizeSamples() : 512));
    player.audioDeviceAboutToStart(d);
}

void PlaybackChain::audioDeviceStopped()
{
    // A pending arm would otherwise start the take on a restarted device, possibly at another rate: fail it.
    const bool wasArmed = armed.exchange(false, std::memory_order_acq_rel);
    if (auto* r = recorder.load(); r != nullptr && (wasArmed || r->isCapturing()))
        r->deviceStopped();
    player.audioDeviceStopped();
}

void PlaybackChain::startArmed() noexcept
{
    // The message thread may have stopped (disarm() saw no arm, then engine.stop()) since consumeArm(). The
    // engine decides atomically: the start applies only to the transport state prepareStart() left, so a
    // command issued after arming wins, and then neither playback nor the take begins.
    if (!engine.startFromAudioThread())
        return;
    resampler.flushBuffers(); // only this thread takes its callbackLock while the device runs
    if (auto* r = recorder.load(std::memory_order_acquire))
        r->begin();
}

void PlaybackChain::audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out,
                                                     int numOut, int n,
                                                     const juce::AudioIODeviceCallbackContext& ctx)
{
    auto* r = recorder.load(std::memory_order_acquire);
    if (consumeArm())
        startArmed();
    if (r != nullptr)
        r->process(in, numIn, n);
    player.audioDeviceIOCallbackWithContext(in, numIn, out, numOut, n, ctx);
}
} // namespace aligna
