#include "engine/Mixer.h"
#include "engine/StretchCache.h"
#include <catch2/catch_test_macros.hpp>
#include <juce_events/juce_events.h>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace aligna;
using Catch::Matchers::WithinAbs;

namespace
{
AudioData constBuf(float v, int n, int ch = 1)
{
    auto b = std::make_shared<juce::AudioBuffer<float>>(ch, n);
    for (int c = 0; c < ch; ++c)
        for (int i = 0; i < n; ++i)
            b->setSample(c, i, v);
    return b;
}

Clip mkClip(const juce::String& src, SamplePos start, SamplePos len)
{
    Clip c;
    c.id = newId();
    c.sourceId = src;
    c.start = start;
    c.length = len;
    return c;
}

std::shared_ptr<MixSnapshot> mk(std::vector<std::pair<juce::String, AudioData>> srcs, std::vector<Track> tracks)
{
    auto s = std::make_shared<MixSnapshot>();
    for (auto& [id, d] : srcs)
    {
        SourceInfo si;
        si.id = id;
        s->project.sources.push_back(si);
        s->audio[id] = d;
    }
    s->project.tracks = std::move(tracks);
    for (auto& t : s->project.tracks)
        std::stable_sort(t.clips.begin(), t.clips.end(), [](auto& a, auto& b) { return a.start < b.start; });
    return s;
}

juce::AudioBuffer<float> render(const MixSnapshot& s, SamplePos start, int n)
{
    juce::AudioBuffer<float> b(2, n);
    b.clear();
    renderMix(s, start, b, 0, n);
    return b;
}
ExportSettings wavSettings(int depth)
{
    ExportSettings e;
    e.format = depth == 16 ? ExportSettings::Format::Wav16 : ExportSettings::Format::Wav24;
    return e;
}
const float k = static_cast<float>(std::cos(juce::MathConstants<double>::pi / 4));
} // namespace

TEST_CASE("mono clip, pan 0, constant power")
{
    Track t;
    t.clips.push_back(mkClip("a", 10, 50));
    auto s = mk({ { "a", constBuf(0.5f, 100) } }, { t });
    auto b = render(*s, 0, 80);
    CHECK_THAT(b.getSample(0, 9), WithinAbs(0, 1e-7));
    CHECK_THAT(b.getSample(0, 10), WithinAbs(0.5f * k, 1e-6));
    CHECK_THAT(b.getSample(1, 30), WithinAbs(0.5f * k, 1e-6));
    CHECK_THAT(b.getSample(0, 60), WithinAbs(0, 1e-7));
}

TEST_CASE("overlap: later-starting clip covers earlier")
{
    Track t;
    t.clips.push_back(mkClip("a", 0, 100));
    t.clips.push_back(mkClip("b", 50, 100));
    auto s = mk({ { "a", constBuf(0.25f, 100) }, { "b", constBuf(1.0f, 100) } }, { t });
    auto b = render(*s, 0, 200);
    CHECK_THAT(b.getSample(0, 40), WithinAbs(0.25f * k, 1e-6));
    CHECK_THAT(b.getSample(0, 60), WithinAbs(1.0f * k, 1e-6));
    CHECK_THAT(b.getSample(0, 120), WithinAbs(1.0f * k, 1e-6));
    CHECK_THAT(b.getSample(0, 160), WithinAbs(0, 1e-7));
}

TEST_CASE("overlap: earlier long clip resumes after a short later clip; equal start later wins")
{
    Track t;
    t.clips.push_back(mkClip("a", 0, 100));
    t.clips.push_back(mkClip("b", 20, 10));
    t.clips.push_back(mkClip("c", 60, 10));
    t.clips.push_back(mkClip("d", 60, 5)); // same start as c, later in vector
    auto s = mk({ { "a", constBuf(0.25f, 100) }, { "b", constBuf(1.0f, 100) },
                  { "c", constBuf(0.5f, 100) }, { "d", constBuf(0.75f, 100) } }, { t });
    auto b = render(*s, 0, 100);
    CHECK_THAT(b.getSample(0, 10), WithinAbs(0.25f * k, 1e-6));
    CHECK_THAT(b.getSample(0, 25), WithinAbs(1.0f * k, 1e-6));
    CHECK_THAT(b.getSample(0, 40), WithinAbs(0.25f * k, 1e-6));
    CHECK_THAT(b.getSample(0, 62), WithinAbs(0.75f * k, 1e-6));
    CHECK_THAT(b.getSample(0, 67), WithinAbs(0.5f * k, 1e-6));
    CHECK_THAT(b.getSample(0, 80), WithinAbs(0.25f * k, 1e-6));
}

TEST_CASE("gain, fades")
{
    Track t;
    auto c = mkClip("a", 0, 100);
    c.gainDb = -6.0206f;
    t.clips.push_back(c);
    auto s = mk({ { "a", constBuf(1.0f, 100) } }, { t });
    CHECK_THAT(render(*s, 0, 10).getSample(0, 3), WithinAbs(0.5f * k, 1e-4));

    t.clips[0].gainDb = 0;
    t.clips[0].fadeIn = 10;
    t.clips[0].fadeOut = 10;
    s = mk({ { "a", constBuf(1.0f, 100) } }, { t });
    auto b = render(*s, 0, 100);
    CHECK_THAT(b.getSample(0, 0), WithinAbs(0, 1e-7));
    CHECK_THAT(b.getSample(0, 5), WithinAbs(0.5f * k, 1e-6));
    CHECK_THAT(b.getSample(0, 50), WithinAbs(k, 1e-6));
    CHECK_THAT(b.getSample(0, 99), WithinAbs(0, 1e-7));
}

TEST_CASE("mute and solo")
{
    Track t1, t2;
    t1.clips.push_back(mkClip("a", 0, 10));
    t2.clips.push_back(mkClip("a", 0, 10));
    auto src = std::make_pair(juce::String("a"), constBuf(0.5f, 10));
    t1.mute = true;
    auto s = mk({ src }, { t1, t2 });
    CHECK_THAT(render(*s, 0, 10).getSample(0, 2), WithinAbs(0.5f * k, 1e-6)); // only t2
    t1.mute = false;
    t2.solo = true;
    s = mk({ src }, { t1, t2 });
    CHECK_THAT(render(*s, 0, 10).getSample(0, 2), WithinAbs(0.5f * k, 1e-6)); // t1 silenced, not doubled
    t2.mute = true;
    s = mk({ src }, { t1, t2 });
    CHECK_THAT(render(*s, 0, 10).getSample(0, 2), WithinAbs(0, 1e-7));
}

TEST_CASE("stereo balance")
{
    Track t;
    t.pan = 0.5f;
    t.clips.push_back(mkClip("a", 0, 10));
    auto s = mk({ { "a", constBuf(1.0f, 10, 2) } }, { t });
    auto b = render(*s, 0, 10);
    CHECK_THAT(b.getSample(0, 1), WithinAbs(0.5, 1e-6));
    CHECK_THAT(b.getSample(1, 1), WithinAbs(1.0, 1e-6));
}

TEST_CASE("block straddling a clip boundary equals split blocks")
{
    Track t;
    t.clips.push_back(mkClip("a", 30, 40));
    t.clips.push_back(mkClip("b", 50, 40));
    auto s = mk({ { "a", constBuf(0.3f, 100) }, { "b", constBuf(0.6f, 100) } }, { t });
    auto whole = render(*s, 0, 100);
    juce::AudioBuffer<float> parts(2, 100);
    parts.clear();
    renderMix(*s, 0, parts, 0, 45);
    renderMix(*s, 45, parts, 45, 55);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 100; ++i)
            CHECK(whole.getSample(c, i) == parts.getSample(c, i));
}

TEST_CASE("missing source and out-of-range reads are safe")
{
    Track t;
    t.clips.push_back(mkClip("gone", 0, 50));
    auto s = mk({}, { t });
    auto b = render(*s, 0, 50);
    CHECK(b.getMagnitude(0, 50) == 0.0f);

    // clip longer than source, offset beyond source end, negative offset
    Track t2;
    auto c1 = mkClip("a", 0, 100);
    c1.sourceOffset = 5;
    auto c2 = mkClip("a", 200, 50);
    c2.sourceOffset = 1000;
    auto c3 = mkClip("a", 300, 20);
    c3.sourceOffset = -5;
    t2.clips = { c1, c2, c3 };
    auto s2 = mk({ { "a", constBuf(1.0f, 20) } }, { t2 });
    auto b2 = render(*s2, 0, 400);
    CHECK_THAT(b2.getSample(0, 10), WithinAbs(k, 1e-6));
    CHECK_THAT(b2.getSample(0, 16), WithinAbs(0, 1e-7)); // 5+16 >= 20
    CHECK_THAT(b2.getSample(0, 220), WithinAbs(0, 1e-7));
    CHECK_THAT(b2.getSample(0, 302), WithinAbs(0, 1e-7)); // negative source index
    CHECK_THAT(b2.getSample(0, 306), WithinAbs(k, 1e-6));
}

TEST_CASE("exportMix writes 24-bit wav matching renderMix")
{
    Track t;
    t.clips.push_back(mkClip("a", 10, 5000));
    auto s = mk({ { "a", constBuf(0.5f, 6000) } }, { t });
    auto f = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("aligna_mix_test.wav");
    f.deleteFile();
    juce::String err;
    float last = 0;
    REQUIRE(exportMix(*s, f, wavSettings(24), err, [&](float p) { last = p; return true; }));
    CHECK(last == 1.0f);
    CHECK_FALSE(f.getSiblingFile(f.getFileName() + ".tmp").exists());

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
    REQUIRE(r != nullptr);
    CHECK(r->lengthInSamples == 5010);
    CHECK(r->bitsPerSample == 24);
    juce::AudioBuffer<float> got(2, 5010);
    r->read(&got, 0, 5010, 0, true, true);
    auto want = render(*s, 0, 5010);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 5010; ++i)
            REQUIRE_THAT(got.getSample(c, i), WithinAbs(want.getSample(c, i), 1e-6));
    r.reset();

    // cancel + errors
    auto f2 = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("aligna_mix_cancel.wav");
    f2.deleteFile();
    CHECK_FALSE(exportMix(*s, f2, wavSettings(16), err, [](float) { return false; }));
    CHECK(err == "Export cancelled");
    CHECK_FALSE(f2.exists());
    CHECK_FALSE(f2.getSiblingFile(f2.getFileName() + ".tmp").exists());
    auto empty = mk({}, {});
    CHECK_FALSE(exportMix(*empty, f2, wavSettings(16), err));
    CHECK(err == "Nothing to export");
    f.deleteFile();
}

namespace
{
std::unique_ptr<juce::AudioFormatReader> openAny(const juce::File& f)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    return std::unique_ptr<juce::AudioFormatReader>(fm.createReaderFor(f));
}
juce::File tmpOut(const char* n)
{
    auto f = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(n);
    f.deleteFile();
    return f;
}
std::shared_ptr<MixSnapshot> sineMix(double rate, int n)
{
    auto b = std::make_shared<juce::AudioBuffer<float>>(1, n);
    for (int i = 0; i < n; ++i)
        b->setSample(0, i, 0.3f * std::sin(0.05f * static_cast<float>(i)));
    Track t;
    t.clips.push_back(mkClip("a", 0, n));
    auto s = mk({ { "a", b } }, { t });
    s->project.sampleRate = rate;
    return s;
}
ExportSettings mp3Settings(int kbps = 256, juce::String title = "T")
{
    ExportSettings e;
    e.format = ExportSettings::Format::Mp3;
    e.kbps = kbps;
    e.title = title;
    return e;
}
} // namespace

TEST_CASE("extensionFor")
{
    CHECK(extensionFor(ExportSettings::Format::Wav16) == ".wav");
    CHECK(extensionFor(ExportSettings::Format::Wav24) == ".wav");
    CHECK(extensionFor(ExportSettings::Format::Mp3) == ".mp3");
}

TEST_CASE("exports mp3")
{
    auto s = sineMix(48000, 48000);
    auto f = tmpOut("aligna_mix_test.mp3");
    juce::String err;
    REQUIRE(exportMix(*s, f, mp3Settings(), err));
    CHECK_FALSE(f.getSiblingFile(f.getFileName() + ".tmp").exists());
    auto r = openAny(f);
    REQUIRE(r != nullptr);
    CHECK(r->numChannels == 2);
    CHECK_THAT(r->sampleRate, WithinAbs(48000.0, 0.5));
    CHECK(std::abs(static_cast<int>(r->lengthInSamples) - 48000) <= 2304);
    r.reset();
    f.deleteFile();
}

TEST_CASE("mp3 export of a 96 kHz project is 44.1 kHz with the right duration")
{
    auto s = sineMix(96000, 96000);
    auto f = tmpOut("aligna_mix_96k.mp3");
    juce::String err;
    REQUIRE(exportMix(*s, f, mp3Settings(192), err));
    auto r = openAny(f);
    REQUIRE(r != nullptr);
    CHECK_THAT(r->sampleRate, WithinAbs(44100.0, 0.5));
    CHECK(std::abs(static_cast<int>(r->lengthInSamples) - 44100) <= 2304);
    r.reset();
    f.deleteFile();
}

TEST_CASE("mp3 cancel leaves no file")
{
    auto s = sineMix(48000, 48000);
    auto f = tmpOut("aligna_mix_cancel.mp3");
    juce::String err;
    CHECK_FALSE(exportMix(*s, f, mp3Settings(), err, [](float) { return false; }));
    CHECK(err == "Export cancelled");
    CHECK_FALSE(f.exists());
    CHECK_FALSE(f.getSiblingFile(f.getFileName() + ".tmp").exists());
}

TEST_CASE("mp3 export with odd title and a very short mix")
{
    auto s = sineMix(44100, 100);
    auto f = tmpOut("aligna_mix_short.mp3");
    juce::String err;
    REQUIRE(exportMix(*s, f, mp3Settings(320, juce::String::fromUTF8("Mix \xc3\xb6 1")), err));
    CHECK(openAny(f) != nullptr);
    f.deleteFile();
}

TEST_CASE("makeSnapshot sorts clips stably and collects audio")
{
    Project p;
    SourceInfo si;
    si.id = "a";
    si.missing = true;
    p.sources.push_back(si);
    Track t;
    auto c1 = mkClip("a", 50, 1), c2 = mkClip("a", 10, 1), c3 = mkClip("a", 10, 1);
    t.clips = { c1, c2, c3 };
    p.tracks.push_back(t);
    SourcePool pool;
    StretchCache cache;
    auto s = makeSnapshot(p, pool, cache);
    CHECK(s->project.tracks[0].clips[0].id == c2.id);
    CHECK(s->project.tracks[0].clips[1].id == c3.id);
    CHECK(s->project.tracks[0].clips[2].id == c1.id);
    CHECK(s->audio.empty());
}

namespace
{
// Imports a 1 s mono 48 kHz ramp-modulated sine and returns its SourceInfo.
SourceInfo importTestSource(SourcePool& pool, const juce::File& dir)
{
    juce::AudioBuffer<float> b(1, 48000);
    for (int i = 0; i < 48000; ++i)
        b.setSample(0, i, 0.5f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 330.0 * i / 48000.0));
    const auto f = dir.getChildFile("src.wav");
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os(f.createOutputStream());
        REQUIRE(os != nullptr);
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), 48000, 1, 32, {}, 0));
        REQUIRE(w != nullptr);
        os.release();
        REQUIRE(w->writeFromAudioSampleBuffer(b, 0, 48000));
    }
    juce::String err;
    auto info = pool.import(f, 48000, err);
    REQUIRE(info.has_value());
    return *info;
}

struct StretchFixture
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("aligna_mx_" + juce::Uuid().toString());
    SourcePool pool;
    Project p;
    Clip stretched, plain;
    StretchFixture()
    {
        dir.createDirectory();
        auto info = importTestSource(pool, dir);
        p.sources.push_back(info);
        stretched = mkClip(info.id, 0, 2 * info.length);
        stretched.stretch = 2.0;
        plain = mkClip(info.id, 0, info.length);
        Track t1, t2;
        t1.clips.push_back(stretched);
        t2.clips.push_back(plain);
        p.tracks = { t1, t2 };
    }
    ~StretchFixture() { dir.deleteRecursively(); }
};
} // namespace

TEST_CASE("makeSnapshot: a stretched clip plays the cache buffer")
{
    StretchFixture f;
    f.p.tracks[1].mute = true;
    StretchCache cache;
    cache.require(f.p, f.pool);
    cache.waitForIdle();
    auto buf = cache.get(bufferKey(f.stretched));
    REQUIRE(buf != nullptr);

    auto s = makeSnapshot(f.p, f.pool, cache);
    CHECK(s->audio.count(bufferKey(f.stretched)) == 1);
    auto b = render(*s, 0, 96000);
    for (int i : { 100, 20000, 60000, 95999 })
    {
        CAPTURE(i);
        CHECK_THAT(b.getSample(0, i), WithinAbs(buf->getSample(0, i) * k, 1e-6));
    }
}

TEST_CASE("makeSnapshot: a stretched clip whose buffer is not ready is silent")
{
    StretchFixture f;
    StretchCache cache; // nothing required: buffer not ready
    auto s = makeSnapshot(f.p, f.pool, cache);
    CHECK(s->audio.count(bufferKey(f.stretched)) == 0);

    f.p.tracks[1].mute = true;
    auto silent = makeSnapshot(f.p, f.pool, cache);
    auto b = render(*silent, 0, 96000);
    CHECK_THAT(b.getMagnitude(0, 96000), WithinAbs(0, 1e-9));

    // The unstretched clip of the same source still plays.
    f.p.tracks[1].mute = false;
    f.p.tracks[0].mute = true;
    auto plain = makeSnapshot(f.p, f.pool, cache);
    auto pb = render(*plain, 0, 48000);
    CHECK(pb.getMagnitude(0, 48000) > 0.1f);
}
