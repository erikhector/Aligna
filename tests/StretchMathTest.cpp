#include "edit/ClipOps.h"
#include "edit/DragMath.h"
#include "edit/StretchMath.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace aligna;

TEST_CASE("stretchForEdge scales the ratio by the new length")
{
    Clip c;
    c.start = 1000;
    c.length = 48000;
    c.stretch = 1.0;
    REQUIRE(stretchForEdge(c, 1000 + 96000) == Catch::Approx(2.0));

    c.stretch = 2.0;
    REQUIRE(stretchForEdge(c, 1000 + 24000) == Catch::Approx(1.0));
    REQUIRE(stretchForEdge(c, 1000 + 48000) == Catch::Approx(2.0));
    REQUIRE(stretchForEdge(c, 500) > 0.0); // before the start: one sample, never zero or negative
}

TEST_CASE("clipTempoRatio reports clamping")
{
    Project p; // 120 bpm
    SourceInfo src; src.id = "s"; src.length = 48000; src.numChannels = 1;
    p.sources.push_back(src);
    Clip c; c.id = "c"; c.sourceId = "s"; c.length = 48000;
    bool limited = true;
    REQUIRE(clipTempoRatio(p, c, 100.0, &limited) == Catch::Approx(100.0 / 120.0));
    REQUIRE_FALSE(limited);
    REQUIRE(clipTempoRatio(p, c, 400.0, &limited) == Catch::Approx(400.0 / 120.0));
    REQUIRE_FALSE(limited);
    p.bpm = 40.0;
    REQUIRE(clipTempoRatio(p, c, 400.0, &limited) == Catch::Approx(kMaxStretch));
    REQUIRE(limited);
    p.bpm = 400.0;
    REQUIRE(clipTempoRatio(p, c, 20.0, &limited) == Catch::Approx(kMinStretch)); // 0.05 -> 0.25
    REQUIRE(limited);
}

TEST_CASE("sameClips sees stretch and tempo changes")
{
    Project p;
    SourceInfo src; src.id = "s"; src.length = 48000; src.numChannels = 1;
    p.sources.push_back(src);
    Track t; t.id = "t";
    Clip c; c.id = "c"; c.sourceId = "s"; c.length = 48000;
    t.clips.push_back(c);
    p.tracks.push_back(t);
    Project q = p;
    REQUIRE(setClipStretch(q, "c", 1.5));
    REQUIRE_FALSE(sameClips(p, q));
    Project r = p;
    REQUIRE(setClipTempo(r, "c", 120.0)); // same stretch (1.0), tempo set
    REQUIRE_FALSE(sameClips(p, r));
}

TEST_CASE("TapTempo averages the last intervals")
{
    TapTempo t;
    REQUIRE(t.tap(10.0) == 0.0); // one tap: no tempo
    REQUIRE(t.tap(10.5) == Catch::Approx(120.0));
    REQUIRE(t.tap(11.0) == Catch::Approx(120.0));
    REQUIRE(t.tap(11.6) == Catch::Approx(60.0 / (1.6 / 3.0)));

    // A gap of 2.5 s starts over.
    REQUIRE(t.tap(14.1) == 0.0);
    REQUIRE(t.tap(14.6) == Catch::Approx(120.0));
}

TEST_CASE("TapTempo uses only the last 8 taps")
{
    TapTempo t;
    double bpm = 0.0;
    for (int i = 0; i < 4; ++i)
        bpm = t.tap(i * 1.0); // 60 bpm
    for (int i = 0; i < 8; ++i)
        bpm = t.tap(3.0 + (i + 1) * 0.5); // 120 bpm; 8 taps drop the slow ones
    REQUIRE(bpm == Catch::Approx(120.0));
    t.reset();
    REQUIRE(t.tap(0.0) == 0.0);
}
