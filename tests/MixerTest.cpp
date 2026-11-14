#include "engine/Mixer.h"
#include "engine/StretchCache.h"
#include "engine/Stretcher.h"
#include "edit/ClipOps.h"
#include <atomic>
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

TEST_CASE("makeSnapshot: a stretched clip whose render failed is silent and listed as failed")
{
    StretchFixture f;
    StretchCache cache(size_t(2) << 30,
                       [](const juce::AudioBuffer<float>&, SamplePos, SamplePos, double, double,
                          const std::function<bool(float)>&) -> std::optional<juce::AudioBuffer<float>> {
                           return std::nullopt;
                       });
    const auto key = bufferKey(f.stretched);

    auto before = makeSnapshot(f.p, f.pool, cache);
    CHECK(before->failedStretch.empty()); // not rendered yet is not "failed"

    cache.require(f.p, f.pool);
    cache.waitForIdle();
    REQUIRE(cache.hasFailed(key));
    CHECK(countStretching(f.p, f.pool, cache) == 0);

    auto s = makeSnapshot(f.p, f.pool, cache);
    CHECK(s->audio.count(key) == 0);
    CHECK(s->failedStretch.count(key) == 1);
    CHECK(s->failedStretch.size() == 1);

    // Export is not blocked: the failed clip is silent, the plain clip of the other track is there.
    auto out = tmpOut("aligna_mix_failed_stretch.wav");
    juce::String err;
    REQUIRE(exportMix(*s, out, wavSettings(16), err));
    auto r = openAny(out);
    REQUIRE(r != nullptr);
    CHECK(r->lengthInSamples == 96000);
    juce::AudioBuffer<float> got(2, 96000);
    r->read(&got, 0, 96000, 0, true, true);
    CHECK(got.getMagnitude(0, 48000) > 0.1f);
    CHECK_THAT(got.getMagnitude(48000, 48000), WithinAbs(0, 1e-9));
    r.reset();
    out.deleteFile();
}

TEST_CASE("exportMix replaces an existing file and leaves no tmp")
{
    for (bool mp3 : { false, true })
    {
        CAPTURE(mp3);
        auto s = sineMix(48000, 24000);
        auto f = tmpOut(mp3 ? "aligna_mix_replace.mp3" : "aligna_mix_replace.wav");
        REQUIRE(f.replaceWithText("the previous export"));
        juce::String err;
        REQUIRE(exportMix(*s, f, mp3 ? mp3Settings() : wavSettings(24), err));
        CHECK_FALSE(f.getSiblingFile(f.getFileName() + ".tmp").exists());
        auto r = openAny(f);
        REQUIRE(r != nullptr);
        if (! mp3)
            CHECK(r->lengthInSamples == 24000);
        r.reset();
        f.deleteFile();
    }
}

TEST_CASE("exportMix: a failed or cancelled export leaves the existing file untouched")
{
    auto s = sineMix(48000, 24000);
    for (int how = 0; how < 3; ++how)
    {
        CAPTURE(how);
        auto f = tmpOut(how == 0 ? "aligna_mix_keep.wav" : "aligna_mix_keep.mp3");
        REQUIRE(f.replaceWithText("the previous export"));
        juce::String err;
        if (how == 0)
            CHECK_FALSE(exportMix(*s, f, wavSettings(24), err, [](float) { return false; }));
        else if (how == 1)
            CHECK_FALSE(exportMix(*s, f, mp3Settings(), err, [](float) { return false; }));
        else
        {
            // The encoder's own reason reaches the caller instead of a generic message.
            CHECK_FALSE(exportMix(*s, f, mp3Settings(100), err));
            CHECK(err == "Unsupported MP3 bitrate: 100 kbps.");
        }
        CHECK(f.loadFileAsString() == "the previous export");
        CHECK_FALSE(f.getSiblingFile(f.getFileName() + ".tmp").exists());
        f.deleteFile();
    }
}

TEST_CASE("exportMix: when the finished export cannot be put in place, nothing is lost")
{
    for (bool mp3 : { false, true })
    {
        CAPTURE(mp3);
        auto s = sineMix(48000, 24000);
        // The destination is a non-empty directory: it cannot be replaced by a file.
        auto out = juce::File::getSpecialLocation(juce::File::tempDirectory)
                       .getChildFile("aligna_mix_blocked_" + juce::Uuid().toString() + (mp3 ? ".mp3" : ".wav"));
        REQUIRE(out.createDirectory().wasOk());
        auto keep = out.getChildFile("keep.txt");
        REQUIRE(keep.replaceWithText("old"));
        const auto tmp = out.getSiblingFile(out.getFileName() + ".tmp");

        juce::String err;
        CHECK_FALSE(exportMix(*s, out, mp3 ? mp3Settings() : wavSettings(24), err));
        CHECK(keep.loadFileAsString() == "old"); // what was there is still there
        REQUIRE(tmp.existsAsFile());             // and so is the finished render
        CHECK(err.contains(tmp.getFullPathName()));
        juce::AudioFormatManager fm; // by content: the ".tmp" name hides the format
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(tmp.createInputStream()));
        REQUIRE(r != nullptr); // complete and readable
        if (! mp3)
            CHECK(r->lengthInSamples == 24000);
        r.reset();

        tmp.deleteFile();
        out.deleteRecursively();
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Stretched buffers that hold only part of a source
// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("renderMix: a stretched buffer of part of a source plays at its offset; outside it is silent")
{
    // Buffer = stretched-source samples [1000, 1100), holding 1 + index. The clip plays stretched [900, 1300) from
    // timeline 50 on, so only timeline [150, 250) has audio.
    auto buf = std::make_shared<juce::AudioBuffer<float>>(1, 100);
    for (int i = 0; i < 100; ++i)
        buf->setSample(0, i, 1.0f + (float) i);
    auto c = mkClip("a", 50, 400);
    c.stretch = 2.0;
    c.sourceOffset = 900;
    Track t;
    t.clips.push_back(c);
    auto s = mk({}, { t });
    s->audio[bufferKey(c)] = buf;

    for (SamplePos start : { SamplePos(1000), SamplePos(0), SamplePos(1300), SamplePos(5000), SamplePos(-5000) })
    {
        CAPTURE(start);
        s->audioStart[bufferKey(c)] = start;
        s->clipAudio = { { ClipBuffer { buf.get(), start } } };
        CHECK(clipBufferFor(*s, c).start == start);
        CHECK(clipBufferFor(*s, c).buffer == buf.get());
        // In one block and in odd-sized blocks: the same, and never outside the buffer.
        auto whole = render(*s, 0, 600);
        juce::AudioBuffer<float> parts(2, 600);
        parts.clear();
        for (int pos = 0; pos < 600; pos += 37)
            renderMix(*s, pos, parts, pos, std::min(37, 600 - pos));
        for (int i = 0; i < 600; ++i)
        {
            // buffer index of timeline i
            const SamplePos bi = c.sourceOffset + (i - c.start) - start;
            const bool audible = i >= 50 && i < 450 && bi >= 0 && bi < 100;
            const float expected = audible ? (1.0f + (float) bi) * k : 0.0f;
            if (std::abs(whole.getSample(0, i) - expected) > 1e-4f || parts.getSample(0, i) != whole.getSample(0, i))
            {
                CAPTURE(i, bi);
                CHECK_THAT(whole.getSample(0, i), WithinAbs(expected, 1e-4));
                CHECK(parts.getSample(0, i) == whole.getSample(0, i));
            }
        }
        if (start == 1000)
        {
            CHECK_THAT(whole.getSample(0, 150), WithinAbs(1.0f * k, 1e-5));
            CHECK_THAT(whole.getSample(0, 249), WithinAbs(100.0f * k, 1e-4));
            CHECK(whole.getSample(0, 149) == 0.0f);
            CHECK(whole.getSample(0, 250) == 0.0f);
        }
        else // the buffer lies wholly outside what the clip plays
            CHECK(whole.getMagnitude(0, 600) == 0.0f);
    }
}

namespace
{
// A 5 ms 2 kHz burst every half second (the first at 0.25 s), 30 s, mono, 48 kHz.
juce::AudioBuffer<float> clickTrain30()
{
    juce::AudioBuffer<float> b(1, 30 * 48000);
    b.clear();
    for (int at = 12000; at + 240 < b.getNumSamples(); at += 24000)
        for (int i = 0; i < 240; ++i)
            b.setSample(0, at + i, 0.9f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 2000.0 * i / 48000.0));
    return b;
}

// The click train imported into a pool, with one clip at ratio 2 playing its seconds [12, 17) from timeline 1000.
struct RangeFixture
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("aligna_mxr_" + juce::Uuid().toString());
    SourcePool pool;
    Project p;
    Clip clip;
    RangeFixture()
    {
        dir.createDirectory();
        const auto b = clickTrain30();
        const auto f = dir.getChildFile("clicks.wav");
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os(f.createOutputStream());
            REQUIRE(os != nullptr);
            std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), 48000, 1, 32, {}, 0));
            REQUIRE(w != nullptr);
            os.release();
            REQUIRE(w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples()));
        }
        juce::String err;
        auto info = pool.import(f, 48000, err);
        REQUIRE(info.has_value());
        p.sources.push_back(*info);
        clip = mkClip(info->id, 1000, 5 * 48000 * 2);
        clip.stretch = 2.0;
        clip.sourceOffset = 12 * 48000 * 2;
        Track t;
        t.id = newId();
        t.clips.push_back(clip);
        p.tracks = { t };
    }
    ~RangeFixture() { dir.deleteRecursively(); }
};

// What the fake render below puts at index i of a whole-source render.
float wholeValue(SamplePos i) { return 0.001f * (float) (1 + i % 997); }

StretchCache::RenderFn fakeRangeRender(std::atomic<int>& calls, std::atomic<bool>& fail)
{
    return [&calls, &fail](const juce::AudioBuffer<float>& in, SamplePos srcBegin, SamplePos srcEnd, double,
                           double ratio, const std::function<bool(float)>&) -> std::optional<juce::AudioBuffer<float>> {
        ++calls;
        if (fail.load())
            return std::nullopt;
        const auto start = stretchedIndex(srcBegin, ratio);
        juce::AudioBuffer<float> out(in.getNumChannels(), (int) (stretchedIndex(srcEnd, ratio) - start));
        for (int i = 0; i < out.getNumSamples(); ++i)
            out.setSample(0, i, wholeValue(start + i));
        return out;
    };
}

// The lag in [-maxLag, maxLag] at which b[i + lag] best matches a[i] over i in [0, n): 0 if they are aligned.
int bestLag(const float* a, const float* b, int n, int maxLag)
{
    int best = 0;
    double peak = -1.0;
    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        double sum = 0;
        for (int i = 0; i < n; ++i)
            sum += (double) a[i] * b[i + lag];
        if (sum > peak)
        {
            peak = sum;
            best = lag;
        }
    }
    REQUIRE(peak > 0.0);
    return best;
}
} // namespace

TEST_CASE("makeSnapshot: a stretched clip plays a render of part of its source at the right place")
{
    RangeFixture f;
    std::atomic<int> calls { 0 };
    std::atomic<bool> fail { false };
    StretchCache cache(size_t(2) << 30, fakeRangeRender(calls, fail));
    cache.require(f.p, f.pool);
    cache.waitForIdle();
    const auto key = bufferKey(f.clip);
    const auto got = cache.find(key);
    REQUIRE(got.data != nullptr);
    REQUIRE(got.start == 10 * 48000 * 2); // source [10 s, 20 s) of 30 s
    REQUIRE(got.data->getNumSamples() == 10 * 48000 * 2);

    auto s = makeSnapshot(f.p, f.pool, cache);
    CHECK(s->audio.at(key) == got.data);
    CHECK(s->audioStart.at(key) == got.start);
    REQUIRE(s->clipAudio.size() == 1);
    REQUIRE(s->clipAudio[0].size() == 1);
    CHECK(s->clipAudio[0][0].buffer == got.data.get());
    CHECK(s->clipAudio[0][0].start == got.start);
    CHECK(s->partialStretch.empty());
    CHECK(s->failedStretch.empty());

    // Timeline t plays index sourceOffset + (t - start) of the whole stretched source, wherever the buffer begins.
    const int n = (int) f.clip.length + 2000;
    auto b = render(*s, 0, n);
    for (int t : { 0, 999, 1000, 1001, 250000, 480999, 481000, 481500 })
    {
        CAPTURE(t);
        const bool inClip = t >= 1000 && t < 1000 + f.clip.length;
        const float expected = inClip ? wholeValue(f.clip.sourceOffset + (t - 1000)) * k : 0.0f;
        CHECK_THAT(b.getSample(0, t), WithinAbs(expected, 1e-6));
    }

    SECTION("a split clip stays continuous: both halves play the one buffer")
    {
        auto p2 = f.p;
        const SamplePos at = 1000 + 123457;
        auto rightId = splitClip(p2, f.clip.id, at);
        REQUIRE(rightId.has_value());
        cache.require(p2, f.pool);
        CHECK_FALSE(cache.isPending(key));
        cache.waitForIdle();
        CHECK(calls == 1); // nothing rendered again
        auto s2 = makeSnapshot(p2, f.pool, cache);
        REQUIRE(s2->clipAudio[0].size() == 2);
        CHECK(s2->clipAudio[0][0].buffer == s2->clipAudio[0][1].buffer);
        CHECK(s2->clipAudio[0][0].start == s2->clipAudio[0][1].start);
        auto b2 = render(*s2, 0, n);
        for (int i = 0; i < n; ++i)
            if (b2.getSample(0, i) != b.getSample(0, i) || b2.getSample(1, i) != b.getSample(1, i))
            {
                CAPTURE(i);
                FAIL("the split clip does not play what the unsplit clip played");
            }
        // In particular across the split point.
        CHECK(b2.getSample(0, (int) at - 1) == wholeValue(f.clip.sourceOffset + (at - 1 - 1000)) * k);
        CHECK(b2.getSample(0, (int) at) == wholeValue(f.clip.sourceOffset + (at - 1000)) * k);
    }
    SECTION("a clip grown beyond the buffer plays what is there, the rest is silent and it is listed as partial")
    {
        auto p2 = f.p;
        auto& c = p2.tracks[0].clips[0];
        c.length = 12 * 48000 * 2; // source [12 s, 24 s): 4 s more than the buffer holds
        fail = true;               // and the larger render fails
        auto partial = makeSnapshot(p2, f.pool, cache); // before the cache was even asked
        CHECK(partial->partialStretch.count(key) == 1);
        CHECK(partial->failedStretch.empty());
        CHECK(partial->audio.count(key) == 1);
        CHECK(countStretching(p2, f.pool, cache) == 1);

        const SamplePos bufEnd = 1000 + (got.start + got.data->getNumSamples() - c.sourceOffset); // on the timeline
        auto pb = render(*partial, bufEnd - 4096, 8192);
        CHECK_THAT(pb.getSample(0, 4095), WithinAbs(wholeValue(got.start + got.data->getNumSamples() - 1) * k, 1e-6));
        CHECK(pb.getMagnitude(0, 4096, 4096) == 0.0f);
        CHECK(pb.getMagnitude(1, 4096, 4096) == 0.0f);

        cache.require(p2, f.pool);
        cache.waitForIdle();
        REQUIRE(cache.hasFailed(key));
        auto failed = makeSnapshot(p2, f.pool, cache);
        CHECK(failed->failedStretch.count(key) == 1);
        CHECK(failed->partialStretch.empty());
        CHECK(failed->audio.count(key) == 1); // the old buffer still plays
        CHECK(countStretching(p2, f.pool, cache) == 0);
        CHECK(countStretchFailed(p2, f.pool, cache) == 1);
    }
}

TEST_CASE("makeSnapshot: a render of part of a source plays in time with a render of the whole source")
{
    RangeFixture f;
    StretchCache cache; // Rubber Band
    cache.require(f.p, f.pool);
    cache.waitForIdle();
    const auto got = cache.find(bufferKey(f.clip));
    REQUIRE(got.data != nullptr);
    REQUIRE(got.start == 10 * 48000 * 2);
    REQUIRE(got.data->getNumSamples() == 10 * 48000 * 2); // a third of the whole stretched source

    const auto whole = stretchBuffer(*f.pool.get(f.clip.sourceId), 48000, 2.0);
    REQUIRE(whole.has_value());

    auto s = makeSnapshot(f.p, f.pool, cache);
    auto b = render(*s, f.clip.start, (int) f.clip.length); // b[i] plays whole-render index sourceOffset + i
    // Every click the clip plays (12.25 s ... 16.75 s of the source) sits where it does in the whole render.
    int clicks = 0;
    for (SamplePos at = 12000; at < 30 * 48000; at += 24000)
    {
        const SamplePos mid = (at + 120) * 2;
        const SamplePos w0 = mid - 4800, w1 = mid + 4800; // +/- 100 ms around the click
        if (w0 - 300 < f.clip.sourceOffset || w1 + 300 > f.clip.sourceOffset + f.clip.length)
            continue;
        const int lag = bestLag(whole->getReadPointer(0) + w0, b.getReadPointer(0) + (w0 - f.clip.sourceOffset), 9600, 300);
        CAPTURE(at, lag);
        CHECK(std::abs(lag) <= 2);
        ++clicks;
    }
    CHECK(clicks == 10);
    // Same level too: the part render is the same audio, not just the same timing.
    const double rms = whole->getRMSLevel(0, (int) f.clip.sourceOffset, (int) f.clip.length) * k;
    CHECK(rms > 0.001);
    CHECK_THAT(b.getRMSLevel(0, 0, (int) f.clip.length), WithinAbs(rms, 0.1 * rms));
}
