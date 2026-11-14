#pragma once
#include <juce_audio_devices/juce_audio_devices.h>

namespace aligna::test
{
struct FakeDeviceConfig
{
    double sampleRate = 44100.0;
    int bufferSize = 512;
    int numInputs = 0, numOutputs = 2;
    int inputLatency = 0, outputLatency = 0;
};

// A device that never runs a thread: start() calls audioDeviceAboutToStart, the test drives the callback itself.
struct FakeAudioDevice : juce::AudioIODevice
{
    explicit FakeAudioDevice(FakeDeviceConfig c = {}) : juce::AudioIODevice("fake", "fake"), cfg(c) {}

    static juce::StringArray names(const char* prefix, int n)
    {
        juce::StringArray a;
        for (int i = 0; i < n; ++i)
            a.add(juce::String(prefix) + juce::String(i + 1));
        return a;
    }
    static juce::BigInteger bits(int n)
    {
        juce::BigInteger b;
        if (n > 0)
            b.setRange(0, n, true);
        return b;
    }

    juce::StringArray getOutputChannelNames() override { return names("Out ", cfg.numOutputs); }
    juce::StringArray getInputChannelNames() override { return names("In ", cfg.numInputs); }
    juce::Array<double> getAvailableSampleRates() override { return { cfg.sampleRate }; }
    juce::Array<int> getAvailableBufferSizes() override { return { cfg.bufferSize }; }
    int getDefaultBufferSize() override { return cfg.bufferSize; }
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
    int getCurrentBufferSizeSamples() override { return cfg.bufferSize; }
    double getCurrentSampleRate() override { return cfg.sampleRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return bits(cfg.numOutputs); }
    juce::BigInteger getActiveInputChannels() const override { return bits(cfg.numInputs); }
    int getOutputLatencyInSamples() override { return cfg.outputLatency; }
    int getInputLatencyInSamples() override { return cfg.inputLatency; }

    FakeDeviceConfig cfg;
    bool opened = false, playing = false;
    juce::AudioIODeviceCallback* cb = nullptr;
};

struct FakeAudioDeviceType : juce::AudioIODeviceType
{
    explicit FakeAudioDeviceType(FakeDeviceConfig c = {}) : juce::AudioIODeviceType("fake"), cfg(c) {}
    void scanForDevices() override {}
    juce::StringArray getDeviceNames(bool) const override { return { "fake" }; }
    int getDefaultDeviceIndex(bool) const override { return 0; }
    int getIndexOfDevice(juce::AudioIODevice*, bool) const override { return 0; }
    bool hasSeparateInputsAndOutputs() const override { return false; }
    juce::AudioIODevice* createDevice(const juce::String&, const juce::String&) override
    {
        return new FakeAudioDevice(cfg);
    }
    FakeDeviceConfig cfg;
};
} // namespace aligna::test
