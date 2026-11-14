#pragma once
#include "engine/PlaybackEngine.h"
#include "engine/Recorder.h"
#include <atomic>
#include <juce_audio_devices/juce_audio_devices.h>

namespace aligna
{
// PlaybackEngine -> ResamplingAudioSource (projectRate / deviceRate) -> AudioSourcePlayer, as a device callback.
// Optionally feeds the device input to a Recorder, starting it in the same callback as playback (armRecording).
class PlaybackChain : public juce::AudioIODeviceCallback
{
public:
    explicit PlaybackChain(PlaybackEngine&, double initialProjectRate = 48000.0);
    ~PlaybackChain() override;

    // Message thread. Re-registers with the manager so audioDeviceAboutToStart re-runs (ratio + prepare)
    // off the audio thread. With no running device this only records the rate.
    void setProjectRate(double rate, juce::AudioDeviceManager&);
    double getAppliedRatio() const { return appliedRatio.load(); }

    // Message thread, before the callback is registered (or while it is not). nullptr = no recording input.
    void setRecorder(Recorder* r) { recorder.store(r); }
    // Message thread, after engine.prepareStart(). The next callback flushes the resampler, begins the recorder
    // and starts the engine, so input sample 0 of the take is captured in the callback that plays the start.
    void armRecording() { armed.store(true, std::memory_order_release); }
    bool isArmed() const { return armed.load(std::memory_order_acquire); }

    // Set in audioDeviceAboutToStart.
    double getDeviceRate() const { return deviceRate.load(); }
    int getInputLatency() const { return inputLatency.load(); }
    int getOutputLatency() const { return outputLatency.load(); }
    double getChainDelay() const { return chainDelay.load(); } // device samples

    // Device samples from engine sample 0 to its output peak, for a fresh resampler at `ratio`
    // (project / device rate) pulled in blocks of `blockSize`. Allocates; not for the audio thread.
    static double measureResamplerDelay(double ratio, int blockSize);

    void audioDeviceAboutToStart(juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const*, int, int,
                                          const juce::AudioIODeviceCallbackContext&) override;

private:
    PlaybackEngine& engine;
    juce::ResamplingAudioSource resampler;
    juce::AudioSourcePlayer player;
    std::atomic<double> projectRate;
    std::atomic<double> appliedRatio { 1.0 };
    std::atomic<Recorder*> recorder { nullptr };
    std::atomic<bool> armed { false };
    std::atomic<double> deviceRate { 0.0 }, chainDelay { 0.0 };
    std::atomic<int> inputLatency { 0 }, outputLatency { 0 };
};
} // namespace aligna
