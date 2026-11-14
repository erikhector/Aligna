#include "engine/SourcePool.h"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

using namespace aligna;

namespace
{
struct TempDir
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("aligna_sp_" + juce::Uuid().toString());
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
};

juce::File writeWav(const juce::File& f, const juce::AudioBuffer<float>& buf, double rate)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os(f.createOutputStream());
    REQUIRE(os != nullptr);
    std::unique_ptr<juce::AudioFormatWriter> w(
        wav.createWriterFor(os.get(), rate, (unsigned) buf.getNumChannels(), 32, {}, 0));
    REQUIRE(w != nullptr);
    os.release();
    REQUIRE(w->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples()));
    return f;
}
} // namespace

TEST_CASE("44.1k sine resampled to 48k keeps length and pitch")
{
    TempDir t;
    juce::AudioBuffer<float> b(1, 44100);
    for (int i = 0; i < 44100; ++i)
        b.setSample(0, i, std::sin(2.0 * juce::MathConstants<double>::pi * 1000.0 * i / 44100.0));
    auto f = writeWav(t.dir.getChildFile("s.wav"), b, 44100);

    SourcePool pool;
    juce::String err;
    auto info = pool.import(f, 48000, err);
    REQUIRE(info.has_value());
    REQUIRE(std::abs((int) info->length - 48000) <= 2);
    REQUIRE(info->numChannels == 1);
    REQUIRE(info->path == f.getFullPathName());
    auto d = pool.get(info->id);
    REQUIRE(d != nullptr);
    REQUIRE(d->getNumSamples() == info->length);

    std::vector<int> zc;
    auto* p = d->getReadPointer(0);
    for (int i = 20000; i < 28000; ++i)
        if (p[i] < 0.f && p[i + 1] >= 0.f)
            zc.push_back(i);
    REQUIRE(zc.size() > 10);
    for (size_t i = 1; i < zc.size(); ++i)
        REQUIRE(std::abs((zc[i] - zc[i - 1]) - 48) <= 1);
}

TEST_CASE("resampling does not shift an impulse in time")
{
    TempDir t;
    const int N = 10000;
    juce::AudioBuffer<float> b(1, 44100);
    b.clear();
    b.setSample(0, N, 1.0f);
    auto f = writeWav(t.dir.getChildFile("i.wav"), b, 44100);

    SourcePool pool;
    juce::String err;
    auto info = pool.import(f, 48000, err);
    REQUIRE(info.has_value());
    auto d = pool.get(info->id);
    auto* p = d->getReadPointer(0);
    int peak = 0;
    for (int i = 0; i < d->getNumSamples(); ++i)
        if (std::abs(p[i]) > std::abs(p[peak]))
            peak = i;
    REQUIRE(std::abs(peak - (int) std::llround(N * 48000.0 / 44100.0)) <= 1);
}

TEST_CASE("same-rate stereo file is bit exact")
{
    TempDir t;
    juce::AudioBuffer<float> b(2, 1000);
    for (int i = 0; i < 1000; ++i)
    {
        b.setSample(0, i, 0.001f * (float) i - 0.5f);
        b.setSample(1, i, -0.0005f * (float) i);
    }
    auto f = writeWav(t.dir.getChildFile("st.wav"), b, 48000);
    SourcePool pool;
    juce::String err;
    auto info = pool.import(f, 48000, err);
    REQUIRE(info.has_value());
    REQUIRE(info->numChannels == 2);
    REQUIRE(info->length == 1000);
    auto d = pool.get(info->id);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 1000; ++i)
            REQUIRE(d->getSample(c, i) == b.getSample(c, i));
    pool.clear();
    REQUIRE(pool.get(info->id) == nullptr);
}

TEST_CASE("multichannel files mix down to stereo")
{
    TempDir t;
    juce::AudioBuffer<float> b(4, 100);
    b.clear();
    for (int i = 0; i < 100; ++i)
        for (int c = 0; c < 4; ++c)
            b.setSample(c, i, c == 0 ? 0.5f : c == 1 ? 0.25f : c == 2 ? -0.25f : 0.75f);
    auto f = writeWav(t.dir.getChildFile("q.wav"), b, 48000);
    SourcePool pool;
    juce::String err;
    auto info = pool.import(f, 48000, err);
    REQUIRE(info.has_value());
    REQUIRE(info->numChannels == 2);
    auto d = pool.get(info->id);
    REQUIRE(std::abs(d->getSample(0, 10) - 0.125f) < 1e-6f);
    REQUIRE(std::abs(d->getSample(1, 10) - 0.5f) < 1e-6f);
}

TEST_CASE("errors: missing, unsupported, empty")
{
    TempDir t;
    SourcePool pool;
    juce::String err;
    REQUIRE_FALSE(pool.import(t.dir.getChildFile("nope.wav"), 48000, err).has_value());
    REQUIRE(err == "Could not open file");

    auto junk = t.dir.getChildFile("junk.wav");
    junk.replaceWithText("this is not audio at all");
    err.clear();
    REQUIRE_FALSE(pool.import(junk, 48000, err).has_value());
    REQUIRE(err == "Unsupported audio format");

    juce::AudioBuffer<float> empty(1, 0);
    auto e = writeWav(t.dir.getChildFile("empty.wav"), empty, 48000);
    err.clear();
    REQUIRE_FALSE(pool.import(e, 48000, err).has_value());
    REQUIRE(err.isNotEmpty());

    SourceInfo si{ "x", t.dir.getChildFile("gone.wav").getFullPathName(), 5, 1, false };
    REQUIRE_FALSE(pool.load(si, 48000));
    REQUIRE(si.missing);
    REQUIRE(pool.get("x") == nullptr);
}

TEST_CASE("load updates length/channels and clears missing")
{
    TempDir t;
    juce::AudioBuffer<float> b(1, 480);
    b.clear();
    auto f = writeWav(t.dir.getChildFile("a.wav"), b, 48000);
    SourcePool pool;
    SourceInfo si{ "id1", f.getFullPathName(), 1, 2, true };
    REQUIRE(pool.load(si, 48000));
    REQUIRE_FALSE(si.missing);
    REQUIRE(si.length == 480);
    REQUIRE(si.numChannels == 1);
    REQUIRE(pool.get("id1") != nullptr);
}

TEST_CASE("non-finite samples in a float file are zeroed at import, loud ones are kept")
{
    TempDir t;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (int channels : { 1, 2, 3 })
    {
        CAPTURE(channels);
        juce::AudioBuffer<float> b(channels, 1000);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < 1000; ++i)
                b.setSample(c, i, 0.25f);
        b.setSample(0, 10, nan);
        b.setSample(0, 11, inf);
        b.setSample(channels - 1, 12, -inf);
        b.setSample(0, 13, 2.5f); // over full scale but finite: must survive
        auto f = writeWav(t.dir.getChildFile("nan" + juce::String(channels) + ".wav"), b, 48000);

        for (double projectRate : { 48000.0, 44100.0 }) // direct and resampled
        {
            CAPTURE(projectRate);
            SourcePool pool;
            juce::String err;
            auto info = pool.import(f, projectRate, err);
            REQUIRE(info.has_value());
            auto d = pool.get(info->id);
            bool allFinite = true;
            for (int c = 0; c < d->getNumChannels(); ++c)
                for (int i = 0; i < d->getNumSamples(); ++i)
                    allFinite = allFinite && std::isfinite(d->getSample(c, i));
            CHECK(allFinite);
            if (projectRate == 48000.0 && channels <= 2)
            {
                CHECK(d->getSample(0, 9) == 0.25f);
                CHECK(d->getSample(0, 10) == 0.0f);
                CHECK(d->getSample(0, 11) == 0.0f);
                CHECK(d->getSample(channels - 1, 12) == 0.0f);
                CHECK(d->getSample(0, 13) == 2.5f);
            }
        }
    }
}

TEST_CASE("a WAV whose header claims more audio than the file holds loads only what is there")
{
    TempDir t;
    juce::AudioBuffer<float> b(2, 48000);
    for (int i = 0; i < 48000; ++i)
    {
        b.setSample(0, i, 0.1f);
        b.setSample(1, i, -0.1f);
    }
    auto whole = writeWav(t.dir.getChildFile("whole.wav"), b, 48000);

    juce::MemoryBlock bytes;
    REQUIRE(whole.loadFileAsData(bytes));
    auto cut = t.dir.getChildFile("cut.wav");
    REQUIRE(cut.replaceWithData(bytes.getData(), bytes.getSize() / 2)); // header intact, half the payload gone

    SourcePool pool;
    juce::String err;
    CHECK(pool.import(whole, 48000, err).has_value());
    auto info = pool.import(cut, 48000, err);
    REQUIRE(info.has_value());
    CHECK(info->length > 0);
    CHECK(info->length < b.getNumSamples() * 3 / 4); // about half, never the claimed length

    SourceInfo si { "x", cut.getFullPathName(), 5, 1, false };
    CHECK(pool.load(si, 48000));
    CHECK_FALSE(si.missing);
}

TEST_CASE("a tiny WAV claiming more than INT_MAX samples loads only the bytes it has")
{
    TempDir t;
    // 8-bit mono PCM, data chunk length 0xFFFFFFF0 (about 4.29e9 samples), 16 bytes of actual audio.
    const unsigned char header[] = {
        'R', 'I', 'F', 'F', 0xF8, 0xFF, 0xFF, 0xFF, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
        0x80, 0xBB, 0, 0, 0x80, 0xBB, 0, 0, 1, 0, 8, 0,
        'd', 'a', 't', 'a', 0xF0, 0xFF, 0xFF, 0xFF
    };
    juce::MemoryBlock bytes(header, sizeof(header));
    const unsigned char audio[16] = {};
    bytes.append(audio, sizeof(audio));
    auto f = t.dir.getChildFile("huge.wav");
    REQUIRE(f.replaceWithData(bytes.getData(), bytes.getSize()));

    SourcePool pool;
    juce::String err;
    auto info = pool.import(f, 48000, err);
    REQUIRE(info.has_value());
    CHECK(info->length <= (SamplePos) bytes.getSize()); // 8-bit mono: at most one frame per byte of the file
}

TEST_CASE("compressed files are not subject to the payload-size check")
{
    TempDir t;
    juce::AudioBuffer<float> b(2, 96000); // digital silence: FLAC shrinks it far below its PCM size
    b.clear();
    auto f = t.dir.getChildFile("silence.flac");
    {
        juce::FlacAudioFormat flac;
        std::unique_ptr<juce::OutputStream> os(f.createOutputStream());
        REQUIRE(os != nullptr);
        std::unique_ptr<juce::AudioFormatWriter> w(flac.createWriterFor(os.get(), 48000, 2, 16, {}, 0));
        REQUIRE(w != nullptr);
        os.release();
        REQUIRE(w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples()));
    }
    REQUIRE(f.getSize() < 96000 * 2 * 2);

    SourcePool pool;
    juce::String err;
    auto info = pool.import(f, 48000, err);
    REQUIRE(info.has_value());
    CHECK(info->length == 96000);
    CHECK(info->numChannels == 2);
}

namespace
{
// Sine of `seconds` at `rate`, a different pitch per channel.
juce::File writeSine(const juce::File& f, double rate, double seconds, int channels = 1)
{
    const int n = (int) (rate * seconds);
    juce::AudioBuffer<float> b(channels, n);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < n; ++i)
            b.setSample(c, i, 0.5f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * (220.0 + 110.0 * c) * i / rate));
    return writeWav(f, b, rate);
}

bool sameAudio(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
        return false;
    for (int c = 0; c < a.getNumChannels(); ++c)
        if (! std::equal(a.getReadPointer(c), a.getReadPointer(c) + a.getNumSamples(), b.getReadPointer(c)))
            return false;
    return true;
}
} // namespace

TEST_CASE("files decoded on worker threads and stored afterwards equal a synchronous import")
{
    TempDir t;
    const auto resampled = writeSine(t.dir.getChildFile("a.wav"), 44100, 3.0, 2);
    const auto sameRate = writeSine(t.dir.getChildFile("b.wav"), 48000, 2.0);

    // Two decodes at the same time, neither touching a pool.
    DecodedAudio da, db;
    std::thread ta([&] { da = SourcePool::decodeFile(resampled, 48000); });
    std::thread tb([&] { db = SourcePool::decodeFile(sameRate, 48000); });
    ta.join();
    tb.join();
    REQUIRE(da.audio != nullptr);
    REQUIRE(db.audio != nullptr);
    CHECK(da.error.isEmpty());
    CHECK_FALSE(da.cancelled);

    SourcePool pool;
    CHECK(pool.size() == 0);
    const auto ia = pool.store(resampled, da.audio);
    const auto ib = pool.store(sameRate, db.audio);
    CHECK(pool.size() == 2);
    CHECK(ia.id != ib.id);
    CHECK(ia.path == resampled.getFullPathName());
    CHECK(ia.numChannels == 2);
    CHECK(ia.length == da.audio->getNumSamples());
    CHECK_FALSE(ia.missing);
    CHECK(pool.get(ia.id) == da.audio);
    CHECK(ib.length == 96000);

    SourcePool reference;
    juce::String err;
    const auto ra = reference.import(resampled, 48000, err);
    const auto rb = reference.import(sameRate, 48000, err);
    REQUIRE(ra.has_value());
    REQUIRE(rb.has_value());
    CHECK(sameAudio(*pool.get(ia.id), *reference.get(ra->id)));
    CHECK(sameAudio(*pool.get(ib.id), *reference.get(rb->id)));
}

TEST_CASE("storing a failed decode under an existing source marks it missing and forgets its buffer")
{
    TempDir t;
    const auto f = writeSine(t.dir.getChildFile("a.wav"), 48000, 0.1);
    SourcePool pool;
    SourceInfo si { "id1", f.getFullPathName(), 1, 2, true };
    REQUIRE(pool.store(si, SourcePool::decodeFile(f, 48000).audio));
    CHECK_FALSE(si.missing);
    CHECK(si.length == 4800);
    CHECK(si.numChannels == 1);
    CHECK(pool.get("id1") != nullptr);

    const auto bad = SourcePool::decodeFile(t.dir.getChildFile("nope.wav"), 48000);
    CHECK(bad.audio == nullptr);
    CHECK(bad.error == "Could not open file");
    CHECK_FALSE(bad.cancelled);
    CHECK_FALSE(pool.store(si, bad.audio));
    CHECK(si.missing);
    CHECK(pool.get("id1") == nullptr);
}

TEST_CASE("decode progress rises to 1 and a cancel stops within one step")
{
    TempDir t;
    for (double fileRate : { 48000.0, 44100.0 }) // without and with resampling
    {
        const auto f = writeSine(t.dir.getChildFile("p.wav"), fileRate, 20.0, 2);

        std::vector<float> seen;
        const auto whole = SourcePool::decodeFile(f, 48000, [&seen](float p) {
            seen.push_back(p);
            return true;
        });
        REQUIRE(whole.audio != nullptr);
        REQUIRE(seen.size() > 8); // asked often enough for a prompt cancel
        CHECK(std::is_sorted(seen.begin(), seen.end()));
        CHECK(seen.front() >= 0.0f);
        CHECK(seen.back() >= 1.0f);

        // Cancel at each stage: before anything is read, while reading, and (44.1k) inside the resampler.
        for (size_t stopAt : { (size_t) 0, (size_t) 2, seen.size() / 2, seen.size() - 2 })
        {
            size_t calls = 0;
            const auto d = SourcePool::decodeFile(f, 48000, [&](float) { return calls++ < stopAt; });
            CHECK(d.cancelled);
            CHECK(d.audio == nullptr);
            CHECK(d.error.isEmpty());
            CHECK(calls == stopAt + 1); // not asked again after the refusal
        }
    }
}

TEST_CASE("a batch decodes on a worker thread, reports per-file progress and keeps per-file errors")
{
    TempDir t;
    DecodeBatch batch;
    batch.projectRate = 48000;
    batch.items.push_back({ writeSine(t.dir.getChildFile("a.wav"), 44100, 2.0), nullptr, {} });
    batch.items.push_back({ t.dir.getChildFile("nope.wav"), nullptr, {} });
    batch.items.push_back({ writeSine(t.dir.getChildFile("c.wav"), 48000, 1.0), nullptr, {} });

    std::vector<std::pair<int, float>> seen;
    bool ok = false;
    std::thread worker([&] {
        ok = batch.run([&seen](int index, float p) {
            seen.emplace_back(index, p);
            return true;
        });
    });
    worker.join();
    REQUIRE(ok);
    CHECK(std::is_sorted(seen.begin(), seen.end())); // files in order, each one's fraction rising
    CHECK(seen.front().first == 0);
    CHECK(seen.back().first == 2);
    CHECK(seen.back().second >= 1.0f);

    REQUIRE(batch.items[0].audio != nullptr);
    CHECK(batch.items[1].audio == nullptr);
    CHECK(batch.items[1].error == "Could not open file");
    REQUIRE(batch.items[2].audio != nullptr);

    // The message-thread step.
    SourcePool pool;
    const auto a = pool.store(batch.items[0].file, batch.items[0].audio);
    const auto c = pool.store(batch.items[2].file, batch.items[2].audio);
    CHECK(std::abs((int) a.length - 96000) <= 2);
    CHECK(c.length == 48000);
    CHECK(pool.size() == 2);
}

TEST_CASE("cancelling a batch from another thread returns promptly and leaves nothing to store")
{
    TempDir t;
    DecodeBatch batch;
    batch.projectRate = 48000;
    batch.items.push_back({ writeSine(t.dir.getChildFile("a.wav"), 48000, 1.0), nullptr, {} });
    batch.items.push_back({ writeSine(t.dir.getChildFile("b.wav"), 44100, 60.0, 2), nullptr, {} }); // slow: resampled
    batch.items.push_back({ writeSine(t.dir.getChildFile("c.wav"), 48000, 1.0), nullptr, {} });

    std::atomic<bool> cancel { false }, secondStarted { false };
    std::atomic<int> callsAfterCancel { 0 }, lastIndex { -1 };
    bool ok = true;
    std::thread worker([&] {
        ok = batch.run([&](int index, float) {
            lastIndex = index;
            if (index == 1)
                secondStarted = true;
            if (cancel.load())
            {
                ++callsAfterCancel;
                return false;
            }
            return true;
        });
    });
    while (! secondStarted.load())
        std::this_thread::yield();
    const auto t0 = std::chrono::steady_clock::now();
    cancel = true;
    worker.join();
    const auto waited = std::chrono::steady_clock::now() - t0;

    CHECK_FALSE(ok);
    CHECK(callsAfterCancel.load() == 1);
    CHECK(lastIndex.load() == 1); // the third file was never started
    CHECK(waited < std::chrono::seconds(5)); // one chunk of work, not the rest of the minute of audio
    for (auto& item : batch.items)
        CHECK(item.audio == nullptr); // the file decoded before the cancel is dropped too
}

TEST_CASE("retainOnly forgets the buffers that are not listed")
{
    TempDir t;
    const auto f = writeSine(t.dir.getChildFile("a.wav"), 48000, 0.1);
    SourcePool pool;
    juce::String err;
    const auto a = pool.import(f, 48000, err), b = pool.import(f, 48000, err), c = pool.import(f, 48000, err);
    REQUIRE((a && b && c));
    const auto held = pool.get(b->id); // e.g. a playback snapshot still using it
    pool.retainOnly({ a->id, c->id, "unknown" });
    CHECK(pool.size() == 2);
    CHECK(pool.get(a->id) != nullptr);
    CHECK(pool.get(b->id) == nullptr);
    CHECK(pool.get(c->id) != nullptr);
    CHECK(held->getNumSamples() == 4800);
    pool.retainOnly({});
    CHECK(pool.size() == 0);
}
