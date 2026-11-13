#include "engine/Mixer.h"
#include <catch2/catch_test_macros.hpp>
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
    REQUIRE(exportMix(*s, f, 24, err, [&](float p) { last = p; return true; }));
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
    CHECK_FALSE(exportMix(*s, f2, 16, err, [](float) { return false; }));
    CHECK(err == "Export cancelled");
    CHECK_FALSE(f2.exists());
    CHECK_FALSE(f2.getSiblingFile(f2.getFileName() + ".tmp").exists());
    CHECK_FALSE(exportMix(*s, f2, 32, err));
    CHECK(err == "Unsupported bit depth");
    auto empty = mk({}, {});
    CHECK_FALSE(exportMix(*empty, f2, 16, err));
    CHECK(err == "Nothing to export");
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
    auto s = makeSnapshot(p, pool);
    CHECK(s->project.tracks[0].clips[0].id == c2.id);
    CHECK(s->project.tracks[0].clips[1].id == c3.id);
    CHECK(s->project.tracks[0].clips[2].id == c1.id);
    CHECK(s->audio.empty());
}
