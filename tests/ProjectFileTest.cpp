#include "io/ProjectFile.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#if JUCE_LINUX || JUCE_MAC || JUCE_BSD
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#endif

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

#if JUCE_LINUX || JUCE_MAC || JUCE_BSD
TEST_CASE("save that cannot write the whole file keeps the previous project", "[projectfile]")
{
    TempDir td;
    auto file = td.dir.getChildFile("proj.aligna");
    Project small;
    juce::String err;
    REQUIRE(saveProject(small, file, err));
    const auto before = file.loadFileAsString();

    Project big; // far more than 4096 bytes of JSON
    for (int i = 0; i < 400; ++i)
    {
        Track t;
        t.id = "t" + juce::String(i);
        t.name = "Track " + juce::String(i);
        big.tracks.push_back(t);
    }

    // Stand-in for a full disk: no file may grow past 4096 bytes, so the write comes up short.
    // No Catch macro runs while the limit is in force.
    struct rlimit old {};
    REQUIRE(getrlimit(RLIMIT_FSIZE, &old) == 0);
    auto oldHandler = signal(SIGXFSZ, SIG_IGN);
    struct rlimit tight = old;
    tight.rlim_cur = 4096;
    const bool limited = setrlimit(RLIMIT_FSIZE, &tight) == 0;
    const bool saved = saveProject(big, file, err);
    setrlimit(RLIMIT_FSIZE, &old);
    signal(SIGXFSZ, oldHandler);

    REQUIRE(limited);
    CHECK_FALSE(saved);
    CHECK(err.contains("Could not write"));
    CHECK(file.loadFileAsString() == before);
    CHECK(td.dir.findChildFiles(juce::File::findFilesAndDirectories, false).size() == 1);

    // With the limit gone the same save works and round-trips.
    REQUIRE(saveProject(big, file, err));
    auto q = loadProject(file, err);
    REQUIRE(q.has_value());
    CHECK(q->tracks.size() == 400);
}

TEST_CASE("a source that is not a regular file loads as missing", "[projectfile]")
{
    TempDir td;
    auto fifo = td.dir.getChildFile("pipe.wav");
    REQUIRE(mkfifo(fifo.getFullPathName().toRawUTF8(), 0600) == 0);
    auto f = td.dir.getChildFile("p.aligna");
    juce::String err;
    for (auto path : { "pipe.wav", "/dev/zero", "." })
    {
        writeText(f, baseJson().replace("/x/a.wav", path));
        auto q = loadProject(f, err);
        REQUIRE(q.has_value());
        CHECK(q->sources[0].missing);
    }
    // The project file itself must be a regular file too.
    CHECK_FALSE(loadProject(fifo, err).has_value());
    CHECK(err.contains("File not found"));
}
#endif

TEST_CASE("saved file is UTF-8 with LF line ends and no byte order mark", "[projectfile]")
{
    TempDir td;
    auto file = td.dir.getChildFile("proj.aligna");
    Project p;
    Track t;
    t.id = "t1";
    t.name = juce::String::fromUTF8("Gitarre \xc3\xa4\xc3\xb6"); // byte count differs from character count
    p.tracks.push_back(t);
    juce::String err;
    REQUIRE(saveProject(p, file, err));
    juce::MemoryBlock bytes;
    REQUIRE(file.loadFileAsData(bytes));
    REQUIRE(bytes.getSize() > 3);
    CHECK(bytes[0] == '{');
    CHECK_FALSE(bytes.toString().contains("\r"));
    CHECK(bytes.toString().contains("\n"));
    auto q = loadProject(file, err);
    REQUIRE(q.has_value());
    CHECK(q->tracks[0].name == p.tracks[0].name);
}

TEST_CASE("load rejects deeply nested and oversized files", "[projectfile]")
{
    TempDir td;
    auto f = td.dir.getChildFile("p.aligna");
    juce::String err;

    SECTION("deep arrays")
    {
        writeText(f, juce::String::repeatedString("[", 500000));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("nested"));
    }
    SECTION("deep objects inside a project")
    {
        writeText(f, baseJson("\"extra\":" + juce::String::repeatedString("{\"a\":", 200000) + "1"
                              + juce::String::repeatedString("}", 200000) + ","));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("nested"));
    }
    SECTION("depth just past the limit")
    {
        writeText(f, baseJson("\"extra\":" + juce::String::repeatedString("[", 64) + juce::String::repeatedString("]", 64) + ","));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("nested"));
        writeText(f, baseJson("\"extra\":" + juce::String::repeatedString("[", 63) + juce::String::repeatedString("]", 63) + ","));
        CHECK(loadProject(f, err).has_value());
    }
    SECTION("brackets inside strings do not count")
    {
        auto brackets = juce::String::repeatedString("[{", 100);
        writeText(f, baseJson("\"extra\":\"" + brackets + "\\\"" + brackets + "\\\\\","));
        CHECK(loadProject(f, err).has_value());
        writeText(f, baseJson().replace("\"name\":\"n\"", "\"name\":\"" + brackets + "\""));
        auto q = loadProject(f, err);
        REQUIRE(q.has_value());
        CHECK(q->tracks[0].name == brackets);
    }
    SECTION("a quote of the other kind does not end a string")
    {
        // The parser also takes '...' strings; brackets after the closing quote must be counted.
        writeText(f, "['\"'," + juce::String::repeatedString("[", 100000));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("nested"));
    }
    SECTION("file over 64 MB")
    {
        {
            juce::FileOutputStream out(f);
            REQUIRE(out.openedOk());
            REQUIRE(out.setPosition(64LL * 1024 * 1024)); // sparse: only the last byte is written
            REQUIRE(out.write(" ", 1));
        }
        REQUIRE(f.getSize() > 64LL * 1024 * 1024);
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("64 MB"));
    }
}

TEST_CASE("gains and pan from a file are clamped to the UI ranges", "[projectfile]")
{
    using Catch::Matchers::WithinULP;
    TempDir td;
    auto f = td.dir.getChildFile("p.aligna");
    juce::String err;
    auto json = [](const juce::String& volumeDb, const juce::String& pan, const juce::String& gainDb)
    {
        return baseJson({}, R"({"id":"c","sourceId":"s","start":"0","sourceOffset":"0","length":"10","gainDb":)" + gainDb
                                + R"(,"fadeIn":"0","fadeOut":"0"})")
            .replace("\"volumeDb\":0", "\"volumeDb\":" + volumeDb)
            .replace("\"pan\":0", "\"pan\":" + pan);
    };

    writeText(f, json("120", "7", "120"));
    auto q = loadProject(f, err);
    REQUIRE(q.has_value());
    CHECK_THAT(q->tracks[0].volumeDb, WithinULP(12.0f, 0));
    CHECK_THAT(q->tracks[0].pan, WithinULP(1.0f, 0));
    CHECK_THAT(q->tracks[0].clips[0].gainDb, WithinULP(12.0f, 0));

    // Finite as a double, infinite as a float without the clamp.
    writeText(f, json("1e300", "-1e300", "-1e300"));
    q = loadProject(f, err);
    REQUIRE(q.has_value());
    CHECK_THAT(q->tracks[0].volumeDb, WithinULP(12.0f, 0));
    CHECK_THAT(q->tracks[0].pan, WithinULP(-1.0f, 0));
    CHECK_THAT(q->tracks[0].clips[0].gainDb, WithinULP(-60.0f, 0));

    // In-range values, the limits included, are untouched.
    writeText(f, json("-60", "-0.5", "-6.5"));
    q = loadProject(f, err);
    REQUIRE(q.has_value());
    CHECK_THAT(q->tracks[0].volumeDb, WithinULP(-60.0f, 0));
    CHECK_THAT(q->tracks[0].pan, WithinULP(-0.5f, 0));
    CHECK_THAT(q->tracks[0].clips[0].gainDb, WithinULP(-6.5f, 0));
}

TEST_CASE("load rejects hostile structure", "[projectfile]")
{
    TempDir td;
    auto f = td.dir.getChildFile("p.aligna");
    juce::String err;
    const juce::String clip = R"({"id":"c","sourceId":"s","start":"0","sourceOffset":"0","length":"10","gainDb":0,"fadeIn":"0","fadeOut":"0"})";
    const juce::String source = R"({"id":"s","path":"/x/a.wav","length":"100","numChannels":1})";
    const juce::String track = R"({"id":"t","name":"n","volumeDb":0,"pan":0,"mute":false,"solo":false,"clips":[]})";
    const juce::String maxPos = "33177600000"; // 24 hours at 384 kHz

    SECTION("zero clip length")
    {
        writeText(f, baseJson({}, clip.replace("\"length\":\"10\"", "\"length\":\"0\"")));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("length"));
        writeText(f, baseJson({}, clip.replace("\"length\":\"10\"", "\"length\":\"1\"")));
        CHECK(loadProject(f, err).has_value());
    }
    SECTION("positions beyond 24 hours at 384 kHz")
    {
        for (juce::String key : { "start", "sourceOffset", "length", "fadeIn", "fadeOut" })
        {
            const auto k = "\"" + key + "\":\"";
            writeText(f, baseJson({}, clip.replace(k + (key == "length" ? "10" : "0") + "\"", k + "33177600001\"")));
            CHECK_FALSE(loadProject(f, err).has_value());
            CHECK(err.contains(key));
            CHECK(err.contains("too large"));
        }
        writeText(f, baseJson({}, clip.replace("\"start\":\"0\"", "\"start\":\"999999999999999999\"")));
        CHECK_FALSE(loadProject(f, err).has_value());
        writeText(f, baseJson().replace("\"length\":\"100\"", "\"length\":\"33177600001\""));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("too large"));

        // The bound itself is accepted, and the project end cannot overflow.
        writeText(f, baseJson({}, clip.replace("\"start\":\"0\"", "\"start\":\"" + maxPos + "\"")
                                      .replace("\"length\":\"10\"", "\"length\":\"" + maxPos + "\"")));
        auto q = loadProject(f, err);
        REQUIRE(q.has_value());
        CHECK(projectEnd(*q) == 2 * 33177600000LL);
    }
    SECTION("duplicate source id")
    {
        writeText(f, baseJson().replace(source, source + "," + source.replace("/x/a.wav", "/x/b.wav")));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("Duplicate source id"));
    }
    SECTION("duplicate track id")
    {
        writeText(f, baseJson().replace("\"tracks\":[", "\"tracks\":[" + track + ","));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("Duplicate track id"));
    }
    SECTION("duplicate clip id")
    {
        writeText(f, baseJson({}, clip + "," + clip));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("Duplicate clip id"));
        // Also across tracks.
        writeText(f, baseJson().replace("\"tracks\":[", "\"tracks\":[" + track.replace("\"id\":\"t\"", "\"id\":\"t0\"")
                                                                             .replace("\"clips\":[]", "\"clips\":[" + clip + "]") + ","));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("Duplicate clip id"));
    }
    SECTION("too many sources")
    {
        juce::StringArray many;
        for (int i = 0; i < 10000; ++i)
            many.add(source.replace("\"id\":\"s\"", "\"id\":\"s" + juce::String(i) + "\""));
        writeText(f, baseJson().replace(source, source + "," + many.joinIntoString(",")));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("Too many sources"));
    }
    SECTION("too many tracks")
    {
        juce::StringArray many;
        for (int i = 0; i < 10000; ++i)
            many.add(track.replace("\"id\":\"t\"", "\"id\":\"t" + juce::String(i) + "\""));
        writeText(f, baseJson().replace("\"tracks\":[", "\"tracks\":[" + many.joinIntoString(",") + ","));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("Too many tracks"));
        many.remove(0); // exactly 10,000 with the baseline track
        writeText(f, baseJson().replace("\"tracks\":[", "\"tracks\":[" + many.joinIntoString(",") + ","));
        auto q = loadProject(f, err);
        REQUIRE(q.has_value());
        CHECK(q->tracks.size() == 10000);
    }
    SECTION("too many clips")
    {
        juce::MemoryOutputStream clips;
        for (int i = 0; i < 1000001; ++i)
            clips << (i == 0 ? "" : ",") << "{}";
        writeText(f, baseJson({}, clips.toString()));
        CHECK_FALSE(loadProject(f, err).has_value());
        CHECK(err.contains("Too many clips"));
    }
}

TEST_CASE("network paths are never looked up", "[projectfile]")
{
    TempDir td;
    auto f = td.dir.getChildFile("p.aligna");
    juce::String err;
    // As written in the JSON text, so a backslash is doubled.
    for (juce::String path : { "//host/share/a.wav", "\\\\\\\\host\\\\share\\\\a.wav", "\\\\/host/share/a.wav", "//tmp" })
    {
        writeText(f, baseJson().replace("/x/a.wav", path));
        auto q = loadProject(f, err);
        REQUIRE(q.has_value());
        CHECK(q->sources[0].missing);
        CHECK(q->sources[0].path == path.replace("\\\\", "\\")); // kept exactly as the file has it
    }
}
