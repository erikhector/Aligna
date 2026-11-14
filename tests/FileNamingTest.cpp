#include <catch2/catch_test_macros.hpp>
#include "io/FileNaming.h"

using namespace aligna;

TEST_CASE("window title")
{
    const auto dash = juce::String(juce::CharPointer_UTF8("\xE2\x80\x94"));
    REQUIRE(windowTitle({}, false) == "Aligna " + dash + " Untitled");
    REQUIRE(windowTitle({}, true) == "Aligna " + dash + " Untitled *");
    REQUIRE(windowTitle("song.aligna", false) == "Aligna " + dash + " song.aligna");
    REQUIRE(windowTitle("song.aligna", true) == "Aligna " + dash + " song.aligna *");
}

TEST_CASE("withExtension appends only when missing")
{
    REQUIRE(withExtension("a", ".aligna") == "a.aligna");
    REQUIRE(withExtension("a.aligna", ".aligna") == "a.aligna");
    REQUIRE(withExtension("a.ALIGNA", ".aligna") == "a.ALIGNA");
    REQUIRE(withExtension("a.b", ".wav") == "a.b.wav");
    REQUIRE(withExtension("a.wav", ".wav") == "a.wav");
    REQUIRE(withExtension("a", "wav") == "a.wav");
}

TEST_CASE("exportFileName swaps wav/mp3 extensions")
{
    REQUIRE(exportFileName("song.wav", ".mp3") == "song.mp3");
    REQUIRE(exportFileName("song.MP3", ".wav") == "song.wav");
    REQUIRE(exportFileName("song.mp3", ".mp3") == "song.mp3");
    REQUIRE(exportFileName("song", ".mp3") == "song.mp3");
    REQUIRE(exportFileName("a.b", ".wav") == "a.b.wav");
}

TEST_CASE("missing files message")
{
    REQUIRE(missingFilesMessage({}).isEmpty());
    REQUIRE(missingFilesMessage({ "/a.wav", "/b.wav" }) == "These audio files could not be found:\n/a.wav\n/b.wav");

    std::vector<juce::String> many;
    for (int i = 0; i < 20; ++i)
        many.push_back("/f" + juce::String(i));
    REQUIRE(! missingFilesMessage(many).contains("more"));
    many.push_back("/f20");
    many.push_back("/f21");
    const auto m = missingFilesMessage(many);
    REQUIRE(m.contains("/f19"));
    REQUIRE(! m.contains("/f20"));
    REQUIRE(m.endsWith(juce::String(juce::CharPointer_UTF8("\xE2\x80\xA6")) + "and 2 more"));
}

TEST_CASE("isProjectSource matches source paths only")
{
    Project p;
    SourceInfo a;
    a.id = "a";
    a.path = "/music/take1.wav";
    p.sources.push_back(a);
    REQUIRE(isProjectSource(p, juce::File("/music/take1.wav")));
    REQUIRE(! isProjectSource(p, juce::File("/music/take1 mix.wav")));
    REQUIRE(! isProjectSource(p, juce::File("/other/take1.wav")));
    REQUIRE(! isProjectSource(Project {}, juce::File("/music/take1.wav")));
}
