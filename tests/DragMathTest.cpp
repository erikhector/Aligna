#include "edit/DragMath.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace aligna;

TEST_CASE("snapStep: bar and subdivisions")
{
    Project p; // 48000 Hz, 120 bpm, 4/4 -> 24000 samples per beat
    REQUIRE(snapStep(p, 1) == Catch::Approx(24000.0));
    REQUIRE(snapStep(p, 2) == Catch::Approx(12000.0));
    REQUIRE(snapStep(p, 4) == Catch::Approx(6000.0));
    REQUIRE(snapStep(p, 0) == Catch::Approx(96000.0));
    p.beatsPerBar = 3;
    REQUIRE(snapStep(p, 0) == Catch::Approx(72000.0));
}

TEST_CASE("computeMoveDelta snaps the anchor's new start")
{
    std::vector<DragClip> c { { 1000, 0 }, { 50000, 1 } };
    auto d = computeMoveDelta(c, 0, 22000, 0, 2, true, 24000.0);
    REQUIRE(d.time == 24000 - 1000); // anchor lands on beat 1
    REQUIRE(d.tracks == 0);
    // applied to the other clip as the same delta
    REQUIRE(c[1].start + d.time == 73000);
    // no snap
    d = computeMoveDelta(c, 0, 22000, 0, 2, false, 24000.0);
    REQUIRE(d.time == 22000);
    // snap with the second clip as the anchor
    c[0].start = 30000;
    d = computeMoveDelta(c, 1, 100, 0, 2, true, 24000.0);
    REQUIRE(d.time == 48000 - 50000);
}

TEST_CASE("computeMoveDelta never moves any clip before 0")
{
    std::vector<DragClip> c { { 5000, 0 }, { 20000, 0 } };
    auto d = computeMoveDelta(c, 1, -30000, 0, 1, false, 0);
    REQUIRE(d.time == -5000);
    d = computeMoveDelta(c, 1, -30000, 0, 1, true, 24000.0); // snap would give -20000 -> clamped
    REQUIRE(d.time == -5000);
}

TEST_CASE("computeMoveDelta limits track delta to the track range")
{
    std::vector<DragClip> c { { 0, 1 }, { 0, 2 } };
    REQUIRE(computeMoveDelta(c, 0, 0, 5, 4, false, 0).tracks == 1);
    REQUIRE(computeMoveDelta(c, 0, 0, -5, 4, false, 0).tracks == -1);
    REQUIRE(computeMoveDelta(c, 0, 0, 0, 4, false, 0).tracks == 0);
    REQUIRE(computeMoveDelta(c, 0, 0, 1, 4, false, 0).tracks == 1);
    REQUIRE(computeMoveDelta({}, 0, 5, 5, 4, false, 0).time == 0);
}

TEST_CASE("limitNudge and snapEdge")
{
    REQUIRE(limitNudge({ 100, 50 }, -480) == -50);
    REQUIRE(limitNudge({ 100, 50 }, 480) == 480);
    REQUIRE(limitNudge({}, 480) == 0);
    REQUIRE(snapEdge(23000, true, 24000.0) == 24000);
    REQUIRE(snapEdge(23000, false, 24000.0) == 23000);
}

#include "edit/ClipOps.h"

TEST_CASE("sameClips detects a trim that clamps back to the original")
{
    Project p;
    SourceInfo src; src.id = "s"; src.length = 100000; src.numChannels = 1;
    p.sources.push_back(src);
    Track t; t.id = "t";
    Clip c; c.id = "c"; c.sourceId = "s"; c.start = 1000; c.sourceOffset = 0; c.length = 50000;
    t.clips.push_back(c);
    p.tracks.push_back(t);

    Project q = p;
    REQUIRE(trimClipStart(q, "c", 0)); // snapped to 0, clamped back to 1000 (sourceOffset 0)
    REQUIRE(sameClips(p, q));

    q = p;
    REQUIRE(trimClipEnd(q, "c", 1000 + 200000)); // beyond source end: clamped to max length 100000
    REQUIRE_FALSE(sameClips(p, q));
    REQUIRE(trimClipEnd(q, "c", 1000 + 300000));
    Project r = q;
    REQUIRE(trimClipEnd(r, "c", 1000 + 400000)); // already at max: no change
    REQUIRE(sameClips(q, r));

    q = p;
    setClipFades(q, "c", 10, 0);
    REQUIRE_FALSE(sameClips(p, q));
}
