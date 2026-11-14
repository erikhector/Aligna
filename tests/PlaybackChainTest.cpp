#include "engine/PlaybackChain.h"
#include "engine/Recorder.h"
#include "support/FakeAudioDevice.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace aligna;
using Catch::Matchers::WithinAbs;
using aligna::test::FakeAudioDeviceType;


TEST_CASE("PlaybackChain re-applies resampling ratio on project rate change", "[engine]")
{
    juce::AudioDeviceManager dm;
    dm.addAudioDeviceType(std::make_unique<FakeAudioDeviceType>());
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

TEST_CASE("measureResamplerDelay is small and stable", "[engine]")
{
    const auto unity = PlaybackChain::measureResamplerDelay(1.0, 480);
    CHECK(unity >= 0.0);
    CHECK(unity <= 8.0);
    const auto down = PlaybackChain::measureResamplerDelay(48000.0 / 44100.0, 480);
    CHECK(down >= 0.0);
    CHECK(down <= 16.0);
    CHECK_THAT(PlaybackChain::measureResamplerDelay(1.0, 480), WithinAbs(unity, 0.0));
    CHECK_THAT(PlaybackChain::measureResamplerDelay(48000.0 / 44100.0, 480), WithinAbs(down, 0.0));
}

TEST_CASE("PlaybackChain reports device rate, latencies and chain delay", "[engine]")
{
    aligna::test::FakeAudioDevice dev({ 44100.0, 480, 1, 2, 100, 156 });
    PlaybackEngine engine;
    PlaybackChain chain(engine, 48000.0);
    chain.audioDeviceAboutToStart(&dev);
    CHECK_THAT(chain.getDeviceRate(), WithinAbs(44100.0, 1e-9));
    CHECK(chain.getInputLatency() == 100);
    CHECK(chain.getOutputLatency() == 156);
    CHECK_THAT(chain.getChainDelay(), WithinAbs(PlaybackChain::measureResamplerDelay(48000.0 / 44100.0, 480), 0.0));
    chain.audioDeviceStopped();
}

TEST_CASE("PlaybackChain arm is consumed by the next callback and starts the engine", "[engine]")
{
    aligna::test::FakeAudioDevice dev({ 48000.0, 64, 1, 2, 0, 0 });
    PlaybackEngine engine;
    PlaybackChain chain(engine, 48000.0);
    chain.audioDeviceAboutToStart(&dev);
    engine.prepareStart(1000);
    chain.armRecording();
    CHECK(chain.isArmed());
    CHECK_FALSE(engine.isPlaying());

    juce::AudioBuffer<float> in(1, 64), out(2, 64);
    in.clear();
    chain.audioDeviceIOCallbackWithContext(in.getArrayOfReadPointers(), 1, out.getArrayOfWritePointers(), 2, 64, {});
    CHECK_FALSE(chain.isArmed());
    CHECK(engine.isPlaying());
    CHECK(engine.getPosition() > 1000);
    engine.stop();
    CHECK(engine.getPosition() == 1000);
    chain.audioDeviceStopped();
}

TEST_CASE("PlaybackChain device stop before a pending arm fires fails the take", "[engine]")
{
    aligna::test::FakeAudioDevice dev({ 48000.0, 64, 1, 2, 0, 0 });
    PlaybackEngine engine;
    PlaybackChain chain(engine, 48000.0);
    auto file = juce::File::createTempFile(".wav");
    Recorder recorder;
    juce::String error;
    REQUIRE(recorder.open(file, 48000.0, 1, error));
    chain.setRecorder(&recorder);
    chain.audioDeviceAboutToStart(&dev);
    engine.prepareStart(1000);
    chain.armRecording();
    REQUIRE(chain.isArmed());

    chain.audioDeviceStopped(); // before any callback consumed the arm
    CHECK_FALSE(chain.isArmed());
    CHECK(recorder.hasFailed());
    CHECK_FALSE(recorder.isCapturing());
    CHECK_FALSE(engine.isPlaying());

    // A restarted device must not start the take.
    chain.audioDeviceAboutToStart(&dev);
    juce::AudioBuffer<float> in(1, 64), out(2, 64);
    in.clear();
    chain.audioDeviceIOCallbackWithContext(in.getArrayOfReadPointers(), 1, out.getArrayOfWritePointers(), 2, 64, {});
    CHECK_FALSE(engine.isPlaying());
    CHECK_FALSE(recorder.isCapturing());
    chain.audioDeviceStopped();
    chain.setRecorder(nullptr);
    recorder.close();
    file.deleteFile();
}
