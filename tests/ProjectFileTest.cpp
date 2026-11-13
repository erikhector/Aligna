#include "io/ProjectFile.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers.hpp>

using namespace aligna;
using Catch::Approx;

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
    CHECK_THAT(q->sampleRate, Catch::Matchers::WithinULP(p.sampleRate, 0));
    CHECK_THAT(q->bpm, Catch::Matchers::WithinULP(p.bpm, 0));
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
        CHECK_THAT(b.volumeDb, Catch::Matchers::WithinULP(a.volumeDb, 0)); CHECK_THAT(b.pan, Catch::Matchers::WithinULP(a.pan, 0));
        CHECK(a.mute == b.mute); CHECK(a.solo == b.solo);
        REQUIRE(a.clips.size() == b.clips.size());
        for (size_t k = 0; k < a.clips.size(); ++k)
        {
            auto &x = a.clips[k], &y = b.clips[k];
            CHECK(x.id == y.id); CHECK(x.sourceId == y.sourceId);
            CHECK(x.start == y.start); CHECK(x.sourceOffset == y.sourceOffset);
            CHECK(x.length == y.length); CHECK_THAT(y.gainDb, Catch::Matchers::WithinULP(x.gainDb, 0));
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
        writeText(f, R"({"format":3})");
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
        writeText(f, baseJson().replace("\"sampleRate\":48000", "\"sampleRate\":7999"));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("sampleRate"));
        writeText(f, baseJson().replace("\"sampleRate\":48000", "\"sampleRate\":384001"));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("sampleRate"));
        writeText(f, baseJson().replace("\"sampleRate\":48000", "\"sampleRate\":1e300"));
        CHECK_FALSE(loadProject(f, err).has_value());
        writeText(f, baseJson().replace("\"sampleRate\":48000", "\"sampleRate\":8000"));
        CHECK(loadProject(f, err).has_value());
        writeText(f, baseJson().replace("\"sampleRate\":48000", "\"sampleRate\":384000"));
        CHECK(loadProject(f, err).has_value());
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

TEST_CASE("failed save keeps previous target and leaves no temp files", "[projectfile]")
{
    TempDir td;
    // Target is a non-empty directory: writing the temp file succeeds, replacing it must fail.
    auto target = td.dir.getChildFile("proj.aligna");
    target.createDirectory();
    auto keep = target.getChildFile("keep.txt");
    keep.replaceWithText("precious");
    Project p;
    juce::String err;
    CHECK_FALSE(saveProject(p, target, err));
    CHECK(err.isNotEmpty());
    CHECK(target.isDirectory());
    CHECK(keep.loadFileAsString() == "precious");
    auto entries = td.dir.findChildFiles(juce::File::findFilesAndDirectories, false);
    CHECK(entries.size() == 1);

    // Missing folder: fails with an error and creates nothing.
    CHECK_FALSE(saveProject(p, td.dir.getChildFile("no/such/dir/p.aligna"), err));
    CHECK(err.isNotEmpty());
    CHECK(td.dir.findChildFiles(juce::File::findFilesAndDirectories, false).size() == 1);
}

TEST_CASE("numChannels validated", "[projectfile]")
{
    TempDir td;
    auto f = td.dir.getChildFile("p.aligna");
    juce::String err;
    for (auto bad : { "0", "3", "-1", "1.5", "1e300" })
    {
        writeText(f, baseJson().replace("\"numChannels\":1", juce::String("\"numChannels\":") + bad));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("numChannels"));
    }
    writeText(f, baseJson().replace("\"format\":1", "\"format\":1e300"));
    CHECK_FALSE(loadProject(f, err).has_value());
    CHECK(err.contains("newer version"));
}

TEST_CASE("format 2 stretch round trip", "[projectfile][stretch]")
{
    TempDir td;
    auto audio = td.dir.getChildFile("a.wav");
    audio.replaceWithText("x");
    auto p = makeProject(audio);
    p.tracks[0].clips[0].stretch = 1.5;
    p.tracks[0].clips[0].sourceBpm = 98;
    auto file = td.dir.getChildFile("proj.aligna");
    juce::String err;
    REQUIRE(saveProject(p, file, err));
    CHECK(file.loadFileAsString().contains("\"format\": 2"));
    auto q = loadProject(file, err);
    REQUIRE(q.has_value());
    CHECK(q->tracks[0].clips[0].stretch == Approx(1.5));
    CHECK(q->tracks[0].clips[0].sourceBpm == Approx(98.0));
    CHECK(q->tracks[0].clips[1].stretch == Approx(1.0));
    CHECK(q->tracks[0].clips[1].sourceBpm == Approx(0.0));
}

TEST_CASE("format 1 loads with stretch defaults", "[projectfile][stretch]")
{
    TempDir td;
    auto f = td.dir.getChildFile("p.aligna");
    juce::String err;
    writeText(f, baseJson());
    auto q = loadProject(f, err);
    REQUIRE(q.has_value());
    CHECK(q->tracks[0].clips[0].stretch == Approx(1.0));
    CHECK(q->tracks[0].clips[0].sourceBpm == Approx(0.0));
}

TEST_CASE("stretch and sourceBpm validated", "[projectfile][stretch]")
{
    TempDir td;
    auto f = td.dir.getChildFile("p.aligna");
    juce::String err;
    auto clipWith = [](const juce::String& extra)
    {
        return R"({"id":"c","sourceId":"s","start":"0","sourceOffset":"0","length":"10","gainDb":0,"fadeIn":"0","fadeOut":"0",)" + extra + "}";
    };
    auto fmt2 = [&](const juce::String& extra) { return baseJson({}, clipWith(extra)).replace("\"format\":1", "\"format\":2"); };

    writeText(f, fmt2(R"("stretch":5.0)"));
    CHECK_FALSE(loadProject(f, err).has_value());
    CHECK(err.contains("stretch"));
    writeText(f, fmt2(R"("stretch":0.1)"));
    CHECK_FALSE(loadProject(f, err).has_value());
    writeText(f, fmt2(R"("sourceBpm":10)"));
    CHECK_FALSE(loadProject(f, err).has_value());
    CHECK(err.contains("sourceBpm"));
    writeText(f, fmt2(R"("sourceBpm":401)"));
    CHECK_FALSE(loadProject(f, err).has_value());
    writeText(f, fmt2(R"("stretch":0.25,"sourceBpm":0)"));
    CHECK(loadProject(f, err).has_value());
}
