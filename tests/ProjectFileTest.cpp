#include "io/ProjectFile.h"
#include <catch2/catch_test_macros.hpp>

using namespace aligna;

namespace
{
struct TempDir
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("aligna_pf_" + juce::Uuid().toString());
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
};

Project makeProject(const juce::File& audio)
{
    Project p;
    p.sampleRate = 44100;
    p.bpm = 97.5;
    p.beatsPerBar = 3;
    p.beatUnit = 8;
    p.sources.push_back({ "s1", audio.getFullPathName(), 6'000'000'000LL, 2, false });
    p.sources.push_back({ "s2", audio.getFullPathName(), 1000, 1, false });
    Track a;
    a.id = "t1"; a.name = "Guitar \"DI\""; a.volumeDb = -3.5f; a.pan = 0.25f; a.mute = true; a.solo = false;
    a.clips.push_back({ "c1", "s1", 5'000'000'000LL, 123, 4'000'000'000LL, -2.f, 100, 200 });
    a.clips.push_back({ "c2", "s2", 0, 0, 1000, 6.f, 0, 0 });
    Track b;
    b.id = "t2"; b.name = "Mic"; b.volumeDb = 0; b.pan = -1.f; b.mute = false; b.solo = true;
    b.clips.push_back({ "c3", "s2", 77, 5, 500, 0.f, 10, 20 });
    p.tracks = { a, b };
    return p;
}

juce::String writeText(const juce::File& f, const juce::String& t)
{
    f.replaceWithText(t);
    return t;
}
} // namespace

TEST_CASE("project round trip", "[projectfile]")
{
    TempDir td;
    auto audio = td.dir.getChildFile("a.wav");
    audio.replaceWithText("x");
    auto p = makeProject(audio);
    auto file = td.dir.getChildFile("proj.aligna");
    juce::String err;
    REQUIRE(saveProject(p, file, err));
    auto q = loadProject(file, err);
    REQUIRE(q.has_value());
    CHECK(q->sampleRate == p.sampleRate);
    CHECK(q->bpm == p.bpm);
    CHECK(q->beatsPerBar == 3);
    CHECK(q->beatUnit == 8);
    REQUIRE(q->sources.size() == 2);
    for (size_t i = 0; i < 2; ++i)
    {
        CHECK(q->sources[i].id == p.sources[i].id);
        CHECK(q->sources[i].path == p.sources[i].path);
        CHECK(q->sources[i].length == p.sources[i].length);
        CHECK(q->sources[i].numChannels == p.sources[i].numChannels);
        CHECK_FALSE(q->sources[i].missing);
    }
    REQUIRE(q->tracks.size() == 2);
    for (size_t i = 0; i < 2; ++i)
    {
        auto &a = p.tracks[i], &b = q->tracks[i];
        CHECK(a.id == b.id); CHECK(a.name == b.name);
        CHECK(a.volumeDb == b.volumeDb); CHECK(a.pan == b.pan);
        CHECK(a.mute == b.mute); CHECK(a.solo == b.solo);
        REQUIRE(a.clips.size() == b.clips.size());
        for (size_t k = 0; k < a.clips.size(); ++k)
        {
            auto &x = a.clips[k], &y = b.clips[k];
            CHECK(x.id == y.id); CHECK(x.sourceId == y.sourceId);
            CHECK(x.start == y.start); CHECK(x.sourceOffset == y.sourceOffset);
            CHECK(x.length == y.length); CHECK(x.gainDb == y.gainDb);
            CHECK(x.fadeIn == y.fadeIn); CHECK(x.fadeOut == y.fadeOut);
        }
    }
    CHECK(q->tracks[0].clips[0].start == 5'000'000'000LL);
}

TEST_CASE("relative path stored for audio in subfolder", "[projectfile]")
{
    TempDir td;
    auto sub = td.dir.getChildFile("audio");
    sub.createDirectory();
    auto audio = sub.getChildFile("a.wav");
    audio.replaceWithText("x");
    Project p;
    p.sources.push_back({ "s1", audio.getFullPathName(), 10, 1, false });
    auto file = td.dir.getChildFile("proj.aligna");
    juce::String err;
    REQUIRE(saveProject(p, file, err));
    auto text = file.loadFileAsString();
    CHECK(text.contains("\"audio/a.wav\""));
    CHECK_FALSE(text.contains(td.dir.getFullPathName().replaceCharacter('\\', '/')));
    auto q = loadProject(file, err);
    REQUIRE(q.has_value());
    CHECK(juce::File(q->sources[0].path) == audio);
    CHECK_FALSE(q->sources[0].missing);
    CHECK_FALSE(text.contains("missing"));
}

TEST_CASE("absolute path stored for audio outside project folder", "[projectfile]")
{
    TempDir td, other;
    auto audio = other.dir.getChildFile("a.wav");
    audio.replaceWithText("x");
    Project p;
    p.sources.push_back({ "s1", audio.getFullPathName(), 10, 1, false });
    auto file = td.dir.getChildFile("proj.aligna");
    juce::String err;
    REQUIRE(saveProject(p, file, err));
    auto q = loadProject(file, err);
    REQUIRE(q.has_value());
    CHECK(juce::File(q->sources[0].path) == audio);
}

TEST_CASE("missing audio loads with missing flag", "[projectfile]")
{
    TempDir td;
    Project p;
    p.sources.push_back({ "s1", td.dir.getChildFile("gone.wav").getFullPathName(), 10, 1, false });
    auto file = td.dir.getChildFile("proj.aligna");
    juce::String err;
    REQUIRE(saveProject(p, file, err));
    auto q = loadProject(file, err);
    REQUIRE(q.has_value());
    CHECK(q->sources[0].missing);
}

TEST_CASE("load rejects bad files", "[projectfile]")
{
    TempDir td;
    auto f = td.dir.getChildFile("p.aligna");
    juce::String err;

    SECTION("newer format")
    {
        writeText(f, R"({"format":2})");
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("newer version"));
    }
    SECTION("garbage")
    {
        writeText(f, "this is {not json");
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.isNotEmpty());
    }
    SECTION("nonexistent file")
    {
        CHECK_FALSE(loadProject(td.dir.getChildFile("nope.aligna"), err).has_value());
        CHECK(err.isNotEmpty());
    }
    SECTION("format missing")
    {
        writeText(f, R"({"sampleRate":48000})");
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("format"));
    }
}

namespace
{
juce::String baseJson(const juce::String& top = {}, const juce::String& clip = {})
{
    return R"({"format":1,"sampleRate":48000,"bpm":120,"beatsPerBar":4,"beatUnit":4,)" + top
           + R"("sources":[{"id":"s","path":"/x/a.wav","length":"100","numChannels":1}],)"
           + R"("tracks":[{"id":"t","name":"n","volumeDb":0,"pan":0,"mute":false,"solo":false,"clips":[)"
           + (clip.isEmpty() ? R"({"id":"c","sourceId":"s","start":"0","sourceOffset":"0","length":"10","gainDb":0,"fadeIn":"0","fadeOut":"0"})" : clip)
           + "]}]}";
}
} // namespace

TEST_CASE("load validation", "[projectfile]")
{
    TempDir td;
    auto f = td.dir.getChildFile("p.aligna");
    juce::String err;

    SECTION("baseline loads, unknown keys ignored")
    {
        writeText(f, baseJson(R"("extra":1,)"));
        CHECK(loadProject(f, err).has_value());
    }
    SECTION("bad bpm")
    {
        writeText(f, baseJson().replace("\"bpm\":120", "\"bpm\":500"));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("bpm"));
        writeText(f, baseJson().replace("\"bpm\":120", "\"bpm\":10"));
        CHECK_FALSE(loadProject(f, err).has_value());
    }
    SECTION("bad beatUnit")
    {
        writeText(f, baseJson().replace("\"beatUnit\":4", "\"beatUnit\":3"));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("beatUnit"));
    }
    SECTION("bad beatsPerBar")
    {
        writeText(f, baseJson().replace("\"beatsPerBar\":4", "\"beatsPerBar\":0"));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("beatsPerBar"));
        writeText(f, baseJson().replace("\"beatsPerBar\":4", "\"beatsPerBar\":33"));
        CHECK_FALSE(loadProject(f, err).has_value());
    }
    SECTION("bad sampleRate")
    {
        writeText(f, baseJson().replace("\"sampleRate\":48000", "\"sampleRate\":0"));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("sampleRate"));
    }
    SECTION("unknown source reference")
    {
        writeText(f, baseJson({}, R"({"id":"c","sourceId":"zzz","start":"0","sourceOffset":"0","length":"10","gainDb":0,"fadeIn":"0","fadeOut":"0"})"));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("zzz"));
    }
    SECTION("non-numeric SamplePos")
    {
        writeText(f, baseJson({}, R"({"id":"c","sourceId":"s","start":"abc","sourceOffset":"0","length":"10","gainDb":0,"fadeIn":"0","fadeOut":"0"})"));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("start"));
    }
    SECTION("missing required key")
    {
        writeText(f, baseJson({}, R"({"id":"c","sourceId":"s","start":"0","sourceOffset":"0","length":"10","fadeIn":"0","fadeOut":"0"})"));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("gainDb"));
    }
    SECTION("missing top-level key")
    {
        writeText(f, baseJson().replace("\"bpm\":120,", ""));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("bpm"));
    }
}

TEST_CASE("failed save keeps previous file", "[projectfile]")
{
    TempDir td;
    auto file = td.dir.getChildFile("proj.aligna");
    Project p;
    juce::String err;
    REQUIRE(saveProject(p, file, err));
    auto before = file.loadFileAsString();
    // Target in a nonexistent directory cannot be written.
    CHECK_FALSE(saveProject(p, td.dir.getChildFile("no/such/dir/p.aligna"), err));
    CHECK(err.isNotEmpty());
    CHECK(file.loadFileAsString() == before);
}
