#include "edit/RecordTiming.h"
#include "engine/PlaybackChain.h"
#include "engine/Recorder.h"
#include "engine/SourcePool.h"
#include "support/FakeAudioDevice.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <memory>

using namespace aligna;

namespace
{
constexpr double projectRate = 48000.0;
constexpr int blockSize = 480, loopDelay = 256, inputLatency = 100, outputLatency = 156;
constexpr SamplePos clickAt = 120000;

std::shared_ptr<const MixSnapshot> clickProject()
{
    const int len = (int) (5 * projectRate);
    auto buf = std::make_shared<juce::AudioBuffer<float>>(1, len);
    buf->clear();
    buf->setSample(0, (int) clickAt, 1.0f);

    auto s = std::make_shared<MixSnapshot>();
    SourceInfo si;
    si.id = "click";
    si.length = len;
    si.numChannels = 1;
    s->project.sources.push_back(si);
    s->audio["click"] = buf;
    Track t;
    t.id = "t";
    Clip c;
    c.id = "c";
    c.sourceId = "click";
    c.start = 0;
    c.length = len;
    t.clips.push_back(c);
    s->project.tracks.push_back(t);
    return s;
}

struct Run
{
    std::vector<float> left; // the chain's left output, every device sample from the arming callback on
    double chainDelay = 0;
};

// Drives a fresh engine + chain the way a device would: aboutToStart, a few idle callbacks, prepareStart(0), arm,
// then `numBlocks` callbacks. The input of device sample n is feed[n - loopDelay] (zero before), or silence without a feed.
Run runChain(double deviceRate, Recorder* recorder, const std::vector<float>* feed, int numBlocks,
             bool stopDevice = false)
{
    test::FakeAudioDevice dev({ deviceRate, blockSize, 1, 2, inputLatency, outputLatency });
    PlaybackEngine engine;
    engine.setSnapshot(clickProject());
    PlaybackChain chain(engine, projectRate);
    chain.setRecorder(recorder);
    chain.audioDeviceAboutToStart(&dev);

    // The device has been running (stopped transport) for a while before Rec is pressed, so the resampler holds
    // read-ahead samples and a sub-sample phase that the arming callback must discard.
    juce::AudioBuffer<float> in(1, blockSize), out(2, blockSize);
    in.clear();
    for (int b = 0; b < 7; ++b)
        chain.audioDeviceIOCallbackWithContext(in.getArrayOfReadPointers(), 1, out.getArrayOfWritePointers(), 2,
                                               blockSize, {});

    engine.prepareStart(0);
    chain.armRecording();

    Run r;
    r.chainDelay = chain.getChainDelay();
    for (int b = 0; b < numBlocks; ++b)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            const auto src = (SamplePos) b * blockSize + i - loopDelay;
            in.setSample(0, i, feed != nullptr && src >= 0 && src < (SamplePos) feed->size() ? (*feed)[(size_t) src] : 0.0f);
        }
        chain.audioDeviceIOCallbackWithContext(in.getArrayOfReadPointers(), 1, out.getArrayOfWritePointers(), 2,
                                               blockSize, {});
        r.left.insert(r.left.end(), out.getReadPointer(0), out.getReadPointer(0) + blockSize);
    }
    if (stopDevice)
        chain.audioDeviceStopped();
    chain.setRecorder(nullptr);
    return r;
}

SamplePos peakIndex(const juce::AudioBuffer<float>& b)
{
    SamplePos best = 0;
    float bestV = -1.0f;
    for (int i = 0; i < b.getNumSamples(); ++i)
        if (std::abs(b.getSample(0, i)) > bestV)
        {
            bestV = std::abs(b.getSample(0, i));
            best = i;
        }
    return best;
}
} // namespace

TEST_CASE("Recorded click lands on the project click", "[loopback]")
{
    const double deviceRate = GENERATE(48000.0, 44100.0);
    const SamplePos recordStart = GENERATE(SamplePos(96000), SamplePos(48000)); // a bar, and half a bar (clamped)
    CAPTURE(deviceRate, recordStart);

    Project p;
    const auto preroll = prerollStart(p, recordStart, true);
    REQUIRE(preroll == 0);

    const int numBlocks = (int) std::ceil(4.0 * deviceRate / blockSize);

    // The chain's output does not depend on its input, so a first pass captures the output stream; the
    // recording pass then hears that stream through the delay line, which keeps the loop causal although the
    // delay (256) is shorter than a block (480). Both passes must produce the same output.
    const auto dry = runChain(deviceRate, nullptr, nullptr, numBlocks);

    auto file = juce::File::createTempFile(".wav");
    Recorder recorder;
    juce::String error;
    REQUIRE(recorder.open(file, deviceRate, 1, error));
    const auto wet = runChain(deviceRate, &recorder, &dry.left, numBlocks);
    const auto res = recorder.close();
    REQUIRE_FALSE(recorder.hasFailed());
    REQUIRE(wet.left == dry.left);
    REQUIRE(res.numSamples == (SamplePos) numBlocks * blockSize);

    SourcePool pool;
    auto info = pool.import(file, projectRate, error);
    REQUIRE(info.has_value());
    auto take = pool.get(info->id);
    REQUIRE(take != nullptr);

    const auto L = totalLatency({ inputLatency, outputLatency, wet.chainDelay, deviceRate, projectRate, 0 });
    const auto placed = placeTake(preroll, recordStart, L, take->getNumSamples(), projectRate);
    REQUIRE(placed.has_value());

    const auto peak = peakIndex(*take);
    const auto clickInTimeline = placed->start + (peak - placed->sourceOffset);
    UNSCOPED_INFO("chainDelay=" << wet.chainDelay << " L=" << L << " peakInTake=" << peak
                                << " error=" << (clickInTimeline - clickAt));
    CHECK(std::abs(clickInTimeline - clickAt) <= 1);
    file.deleteFile();
}

TEST_CASE("device stop mid-take keeps audio", "[loopback]")
{
    const double deviceRate = 48000.0;
    auto file = juce::File::createTempFile(".wav");
    Recorder recorder;
    juce::String error;
    REQUIRE(recorder.open(file, deviceRate, 1, error));
    runChain(deviceRate, &recorder, nullptr, (int) (2.0 * deviceRate / blockSize), true);
    REQUIRE(recorder.hasFailed());
    REQUIRE_FALSE(recorder.isCapturing());
    const auto res = recorder.close();
    CHECK(res.numSamples >= (SamplePos) (1.9 * deviceRate));
    CHECK(res.numSamples <= (SamplePos) (2.1 * deviceRate));

    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::AudioFormatReader> reader(fmt.createReaderFor(file.createInputStream().release(), true));
    REQUIRE(reader != nullptr);
    CHECK(reader->lengthInSamples == res.numSamples);
    reader.reset();
    file.deleteFile();
}
