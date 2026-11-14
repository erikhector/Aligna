#include <catch2/catch_test_macros.hpp>
#include "edit/ClipOps.h"
#include "edit/EditHistory.h"
#include "io/TakeFiles.h"

using namespace aligna;

namespace
{
struct Scratch
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("aligna-takes-" + juce::Uuid().toString());
    Scratch() { dir.createDirectory(); }
    ~Scratch() { dir.deleteRecursively(); }
};

void writeFile(const juce::File& f, const juce::String& text)
{
    f.getParentDirectory().createDirectory();
    REQUIRE(f.replaceWithText(text));
}
} // namespace

TEST_CASE("takes folder for project file", "[takes]")
{
    REQUIRE(takesFolderFor(juce::File("/x/Song.aligna")).getFullPathName()
            == juce::File("/x/Song Takes").getFullPathName());
}

TEST_CASE("new session takes folder naming", "[takes]")
{
    Scratch s;
    const juce::Time t(2026, 10, 14, 9, 5, 7, 0, true);
    auto f = newSessionTakesFolder(s.dir, t);
    REQUIRE(f.getFileName() == "2026-11-14 09-05-07");
    REQUIRE(f.getParentDirectory() == s.dir);
    REQUIRE_FALSE(f.exists());
    f.createDirectory();
    REQUIRE(newSessionTakesFolder(s.dir, t).getFileName() == "2026-11-14 09-05-07 (2)");
}

TEST_CASE("default unsaved takes root", "[takes]")
{
    auto r = defaultUnsavedTakesRoot();
    REQUIRE(r.getFileName() == "Unsaved Takes");
    REQUIRE(r.getParentDirectory().getFileName() == "Aligna");
}

TEST_CASE("unique take file", "[takes]")
{
    Scratch s;
    REQUIRE(uniqueTakeFile(s.dir, "Take 1").getFileName() == "Take 1.wav");
    writeFile(s.dir.getChildFile("Take 1.wav"), "x");
    REQUIRE(uniqueTakeFile(s.dir, "Take 1").getFileName() == "Take 1 (2).wav");
}

TEST_CASE("relocateTakes moves files", "[takes]")
{
    Scratch s;
    auto from = s.dir.getChildFile("from"), to = s.dir.getChildFile("to");
    writeFile(from.getChildFile("Take 1.wav"), "one");
    writeFile(from.getChildFile("Take 2.wav"), "two");
    writeFile(to.getChildFile("Take 1.wav"), "existing");
    juce::String err;
    auto map = relocateTakes(from, to, err);
    REQUIRE(err.isEmpty());
    REQUIRE(map.size() == 2);
    REQUIRE(to.getChildFile("Take 1.wav").loadFileAsString() == "existing");
    REQUIRE(to.getChildFile("Take 1 (2).wav").loadFileAsString() == "one");
    REQUIRE(to.getChildFile("Take 2.wav").loadFileAsString() == "two");
    REQUIRE(from.getNumberOfChildFiles(juce::File::findFiles) == 0);
    REQUIRE(map.at(from.getChildFile("Take 1.wav").getFullPathName())
            == to.getChildFile("Take 1 (2).wav").getFullPathName());
}

TEST_CASE("applyRelocation changes only matching sources", "[takes]")
{
    Project p;
    SourceInfo a, b;
    a.id = "a"; a.path = "/old/a.wav";
    b.id = "b"; b.path = "/other/b.wav";
    p.sources = {a, b};
    applyRelocation(p, {{"/old/a.wav", "/new/a.wav"}});
    REQUIRE(p.sources[0].path == "/new/a.wav");
    REQUIRE(p.sources[1].path == "/other/b.wav");
}

TEST_CASE("undo, save, redo keeps the moved path", "[takes]")
{
    Scratch s;
    auto from = s.dir.getChildFile("from"), to = s.dir.getChildFile("to");
    auto take = from.getChildFile("Take 1.wav");
    writeFile(take, "audio");

    EditHistory hist{Project{}};
    SourceInfo src;
    src.id = "src1"; src.path = take.getFullPathName(); src.length = 48000; src.numChannels = 1;
    TakePlacement pl; pl.start = 0; pl.sourceOffset = 0; pl.length = 48000;
    REQUIRE(hist.perform("Record", [&](Project& p) { addTakeTrack(p, src, "Take 1", pl); return true; }));
    REQUIRE(hist.undo());

    juce::String err;
    auto map = relocateTakes(from, to, err);
    REQUIRE(err.isEmpty());
    hist.rewriteAllStates([&](Project& p) { applyRelocation(p, map); });

    REQUIRE(hist.redo());
    REQUIRE(hist.current().sources.size() == 1);
    auto path = hist.current().sources[0].path;
    REQUIRE(juce::File(path).getParentDirectory() == to);
    REQUIRE(juce::File(path).existsAsFile());
}

TEST_CASE("isDiskAlmostFull treats unknown free space as not full", "[takes]")
{
    constexpr juce::int64 MB = 1024 * 1024;
    constexpr juce::int64 total = 4096 * MB;
    REQUIRE_FALSE(isDiskAlmostFull(0, 0)); // JUCE could not query the volume
    REQUIRE_FALSE(isDiskAlmostFull(0, -1));
    REQUIRE_FALSE(isDiskAlmostFull(-1, 0));
    REQUIRE_FALSE(isDiskAlmostFull(-1, total));
    REQUIRE(isDiskAlmostFull(49 * MB, total));
    REQUIRE(isDiskAlmostFull(49 * MB, 0)); // a known free size is trusted without the total
    REQUIRE_FALSE(isDiskAlmostFull(50 * MB, total));
    REQUIRE_FALSE(isDiskAlmostFull(1024 * MB, total));
}

TEST_CASE("isDiskAlmostFull treats zero free bytes on a known volume as full", "[takes]")
{
    constexpr juce::int64 MB = 1024 * 1024;
    REQUIRE(isDiskAlmostFull(0, 4096 * MB));
    REQUIRE(isDiskAlmostFull(0, 1));
}

TEST_CASE("isDiskAlmostFull(folder) queries the volume, also for a folder that does not exist yet", "[takes]")
{
    const auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto missing = tmp.getChildFile("aligna-no-such-folder-" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    REQUIRE(isDiskAlmostFull(missing) == isDiskAlmostFull(tmp.getBytesFreeOnVolume(), tmp.getVolumeTotalSize()));
}
