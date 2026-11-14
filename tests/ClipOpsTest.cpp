#include "edit/ClipOps.h"
#include <catch2/catch_test_macros.hpp>

using namespace aligna;

namespace
{
struct Fx
{
    Project p;
    juce::String id;
    Fx()
    {
        SourceInfo s;
        s.id = "src";
        s.path = "a.wav";
        s.length = 1000;
        s.numChannels = 1;
        id = addTrackWithClip(p, s, "T1");
        p.tracks.push_back(Track{ "t2", "T2", 0, 0, false, false, {} });
        findClip(p, id)->start = 100;
    }
    Clip& c() { return *findClip(p, id); }
};
} // namespace

TEST_CASE("ClipOps addTrackWithClip", "[ClipOps]")
{
    Project p;
    SourceInfo s;
    s.id = "s";
    s.length = 500;
    auto cid = addTrackWithClip(p, s, "X");
    addTrackWithClip(p, s, "Y");
    REQUIRE(p.sources.size() == 1);
    REQUIRE(p.tracks.size() == 2);
    auto* c = findClip(p, cid);
    REQUIRE(c != nullptr);
    REQUIRE(c->start == 0);
    REQUIRE(c->length == 500);
    REQUIRE(findClip(p, "nope") == nullptr);
}

TEST_CASE("ClipOps moveClip", "[ClipOps]")
{
    Fx f;
    REQUIRE(f.p.tracks.size() == 2);
    REQUIRE(moveClip(f.p, f.id, -50, 0));
    REQUIRE(f.c().start == 0);
    REQUIRE(moveClip(f.p, f.id, 20, 1));
    int ti = -1;
    findClip(f.p, f.id, &ti);
    REQUIRE(ti == 1);
    REQUIRE(f.p.tracks[0].clips.empty());
    REQUIRE_FALSE(moveClip(f.p, f.id, 5, 7));
    REQUIRE_FALSE(moveClip(f.p, f.id, 5, -1));
    REQUIRE_FALSE(moveClip(f.p, "bad", 5, 0));
    REQUIRE(f.c().start == 20);
    REQUIRE(moveClip(f.p, f.id, (SamplePos) 3'000'000'000, 1));
    REQUIRE(f.c().start == 3'000'000'000);
    REQUIRE(projectEnd(f.p) == 3'000'000'000 + 1000);
}

TEST_CASE("ClipOps trimClipStart", "[ClipOps]")
{
    Fx f;
    REQUIRE(trimClipStart(f.p, f.id, 150));
    REQUIRE(f.c().start == 150);
    REQUIRE(f.c().sourceOffset == 50);
    REQUIRE(f.c().length == 950);
    REQUIRE(trimClipStart(f.p, f.id, 0));
    REQUIRE(f.c().start == 100);
    REQUIRE(f.c().sourceOffset == 0);
    REQUIRE(f.c().length == 1000);
    REQUIRE(trimClipStart(f.p, f.id, 100 + 1000));
    REQUIRE(f.c().length == 1);
    REQUIRE(f.c().start == 1099);
    REQUIRE_FALSE(trimClipStart(f.p, "bad", 0));
}

TEST_CASE("ClipOps trimClipEnd", "[ClipOps]")
{
    Fx f;
    REQUIRE(trimClipEnd(f.p, f.id, 5000));
    REQUIRE(f.c().length == 1000);
    REQUIRE(trimClipEnd(f.p, f.id, 0));
    REQUIRE(f.c().length == 1);
    REQUIRE(trimClipEnd(f.p, f.id, 400));
    REQUIRE(f.c().length == 300);
    f.p.sources.clear();
    REQUIRE_FALSE(trimClipEnd(f.p, f.id, 400));
}

TEST_CASE("ClipOps splitClip", "[ClipOps]")
{
    Fx f;
    setClipFades(f.p, f.id, 100, 80);
    auto r = splitClip(f.p, f.id, 600);
    REQUIRE(r.has_value());
    auto& a = f.c();
    auto* b = findClip(f.p, *r);
    REQUIRE(b != nullptr);
    REQUIRE(a.length == 500);
    REQUIRE(b->length == 500);
    REQUIRE(b->start == 600);
    REQUIRE(b->sourceOffset == 500);
    REQUIRE(a.fadeIn == 100);
    REQUIRE(a.fadeOut == 0);
    REQUIRE(b->fadeOut == 80);
    REQUIRE(b->fadeIn == 0);
    REQUIRE(*r != f.id);
    Fx g;
    REQUIRE_FALSE(splitClip(g.p, g.id, 100).has_value());
    REQUIRE_FALSE(splitClip(g.p, g.id, 1100).has_value());
    REQUIRE_FALSE(splitClip(g.p, "bad", 500).has_value());
    REQUIRE(g.p.tracks[0].clips.size() == 1);
}

TEST_CASE("ClipOps fades, gain, delete, removeTrack", "[ClipOps]")
{
    Fx f;
    REQUIRE(setClipFades(f.p, f.id, 2000, 2000));
    REQUIRE(f.c().fadeIn == 500);
    REQUIRE(f.c().fadeOut == 500);
    REQUIRE(f.c().fadeIn + f.c().fadeOut <= f.c().length);
    REQUIRE(setClipFades(f.p, f.id, -5, 10));
    REQUIRE(f.c().fadeIn == 0);
    REQUIRE(setClipGain(f.p, f.id, 100.f));
    REQUIRE(juce::exactlyEqual(f.c().gainDb, 12.0f));
    REQUIRE(setClipGain(f.p, f.id, -100.f));
    REQUIRE(juce::exactlyEqual(f.c().gainDb, -60.0f));
    REQUIRE(trimClipEnd(f.p, f.id, 300));
    REQUIRE(f.c().fadeOut <= f.c().length / 2);
    REQUIRE_FALSE(removeTrack(f.p, 5));
    REQUIRE_FALSE(removeTrack(f.p, -1));
    REQUIRE(removeTrack(f.p, 1));
    REQUIRE(f.p.tracks.size() == 1);
    REQUIRE(deleteClip(f.p, f.id));
    REQUIRE_FALSE(deleteClip(f.p, f.id));
}

TEST_CASE("ClipOps addTakeTrack places the clip", "[ClipOps]")
{
    Project p;
    SourceInfo s;
    s.id = "take";
    s.path = "t.wav";
    s.length = 500000;
    s.numChannels = 1;
    auto id = addTakeTrack(p, s, "Take 1", { 200000, 96480, 403520 });
    REQUIRE(p.tracks.size() == 1);
    CHECK(p.tracks[0].name == "Take 1");
    auto& c = *findClip(p, id);
    CHECK(c.start == 200000);
    CHECK(c.sourceOffset == 96480);
    CHECK(c.length == 403520);
    CHECK(c.stretch == 1.0);
    CHECK(c.gainDb == 0.0f);
    CHECK(p.sources.size() == 1);
}
