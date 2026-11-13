#include "engine/PlaybackChain.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace aligna;
using Catch::Matchers::WithinAbs;

namespace
{
struct FakeDevice : juce::AudioIODevice
{
    FakeDevice() : juce::AudioIODevice("fake", "fake") {}
    juce::StringArray getOutputChannelNames() override { return { "L", "R" }; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return { 44100.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { 512 }; }
    int getDefaultBufferSize() override { return 512; }
    juce::String open(const juce::BigInteger&, const juce::BigInteger&, double, int) override
    {
        opened = true;
        return {};
    }
    void close() override { opened = false; }
    bool isOpen() override { return opened; }
    void start(juce::AudioIODeviceCallback* c) override
    {
        cb = c;
        cb->audioDeviceAboutToStart(this);
        playing = true;
    }
    void stop() override
    {
        if (cb != nullptr)
            cb->audioDeviceStopped();
        cb = nullptr;
        playing = false;
    }
    bool isPlaying() override { return playing; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return 512; }
    double getCurrentSampleRate() override { return 44100.0; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { juce::BigInteger b; b.setRange(0, 2, true); return b; }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }
    bool opened = false, playing = false;
    juce::AudioIODeviceCallback* cb = nullptr;
};

struct FakeType : juce::AudioIODeviceType
{
    FakeType() : juce::AudioIODeviceType("fake") {}
    void scanForDevices() override {}
    juce::StringArray getDeviceNames(bool) const override { return { "fake" }; }
    int getDefaultDeviceIndex(bool) const override { return 0; }
    int getIndexOfDevice(juce::AudioIODevice*, bool) const override { return 0; }
    bool hasSeparateInputsAndOutputs() const override { return false; }
    juce::AudioIODevice* createDevice(const juce::String&, const juce::String&) override { return new FakeDevice(); }
};
} // namespace

TEST_CASE("PlaybackChain re-applies resampling ratio on project rate change", "[engine]")
{
    juce::AudioDeviceManager dm;
    dm.addAudioDeviceType(std::make_unique<FakeType>());
    dm.setCurrentAudioDeviceType("fake", true);
    PlaybackEngine engine;
    PlaybackChain chain(engine, 48000.0);
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.outputDeviceName = "fake";
    setup.sampleRate = 44100.0;
    setup.bufferSize = 512;
    setup.outputChannels.setRange(0, 2, true);
    REQUIRE(dm.setAudioDeviceSetup(setup, false).isEmpty());
    dm.addAudioCallback(&chain);
    REQUIRE_THAT(chain.getAppliedRatio(), WithinAbs(48000.0 / 44100.0, 1e-9));

    chain.setProjectRate(44100.0, dm);
    REQUIRE_THAT(chain.getAppliedRatio(), WithinAbs(1.0, 1e-9));
    chain.setProjectRate(48000.0, dm);
    REQUIRE_THAT(chain.getAppliedRatio(), WithinAbs(48000.0 / 44100.0, 1e-9));
    dm.removeAudioCallback(&chain);
}

TEST_CASE("PlaybackChain setProjectRate with no device is safe", "[engine]")
{
    juce::AudioDeviceManager dm;
    PlaybackEngine engine;
    PlaybackChain chain(engine);
    chain.setProjectRate(44100.0, dm);
    REQUIRE_THAT(chain.getAppliedRatio(), WithinAbs(1.0, 1e-9));
}
