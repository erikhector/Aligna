#pragma once
#include "engine/PlaybackEngine.h"
#include <atomic>
#include <juce_audio_devices/juce_audio_devices.h>

namespace aligna
{
// PlaybackEngine -> ResamplingAudioSource (projectRate / deviceRate) -> AudioSourcePlayer, as a device callback.
class PlaybackChain : public juce::AudioIODeviceCallback
{
public:
    explicit PlaybackChain(PlaybackEngine&, double initialProjectRate = 48000.0);
    ~PlaybackChain() override;

    // Message thread. Re-registers with the manager so audioDeviceAboutToStart re-runs (ratio + prepare)
    // off the audio thread. With no running device this only records the rate.
    void setProjectRate(double rate, juce::AudioDeviceManager&);
    double getAppliedRatio() const { return appliedRatio.load(); }

    void audioDeviceAboutToStart(juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const*, int, int,
                                          const juce::AudioIODeviceCallbackContext&) override;

private:
    juce::ResamplingAudioSource resampler;
    juce::AudioSourcePlayer player;
    std::atomic<double> projectRate;
    std::atomic<double> appliedRatio { 1.0 };
};
} // namespace aligna
