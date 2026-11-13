#include "align/AlignResults.h"
#include "edit/ClipOps.h"
#include <catch2/catch_test_macros.hpp>

using namespace aligna;

namespace
{
AlignRow row(SamplePos oldS, SamplePos newS, float conf, bool inv = false)
{
    AlignRow r;
    r.clipId = "c";
    r.label = "Vox #1";
    r.oldStart = oldS;
    r.result = ClipAlignment { newS, conf, inv };
    return r;
}
Project twoClips()
{
    Project p;
    p.sources.push_back({ "s", "x.wav", 100000, 1, false });
    for (int t = 0; t < 2; ++t)
    {
        Track tr;
        tr.id = "t" + juce::String(t);
        Clip c;
        c.id = "c" + juce::String(t);
        c.sourceId = "s";
        c.start = 1000 * t;
        c.length = 50000;
        tr.clips.push_back(c);
        p.tracks.push_back(tr);
    }
    return p;
}
} // namespace

TEST_CASE("pre-tick rule", "[alignresults]")
{
    REQUIRE(shouldPreTick(row(0, 10, 2.0f)));
    REQUIRE(shouldPreTick(row(0, 10, 9.0f)));
    REQUIRE_FALSE(shouldPreTick(row(0, 10, 1.9f)));
    AlignRow e;
    e.error = "silence";
    REQUIRE_FALSE(shouldPreTick(e));
    auto both = row(0, 10, 5.0f);
    both.error = "x";
    REQUIRE_FALSE(shouldPreTick(both));
}

TEST_CASE("offset formatting", "[alignresults]")
{
    REQUIRE(formatOffsetMs(14400, 48000.0) == "+300.000 ms");
    REQUIRE(formatOffsetMs(-14400, 48000.0) == "-300.000 ms");
    REQUIRE(formatOffsetMs(0, 48000.0) == "+0.000 ms");
    REQUIRE(formatOffsetMs(1, 48000.0) == "+0.021 ms");
    REQUIRE(formatOffsetSamples(-14400) == "-14400 smp");
    REQUIRE(formatOffsetSamples(5) == "+5 smp");
    REQUIRE(formatConfidence(3.456f) == "3.5");
}

TEST_CASE("row description", "[alignresults]")
{
    auto d = describeRow(row(1000, 1000 + 14400, 4.0f), 48000.0);
    REQUIRE(d.contains("+300.000 ms"));
    REQUIRE(d.contains("+14400"));
    REQUIRE(d.contains("4.0"));
    REQUIRE_FALSE(d.contains("polarity"));
    REQUIRE_FALSE(d.contains("low confidence"));
    REQUIRE(describeRow(row(0, 5, 1.0f, true), 48000.0).contains("polarity inverted"));
    REQUIRE(describeRow(row(0, 5, 1.0f, true), 48000.0).contains("low confidence"));
    AlignRow e;
    e.label = "Vox #2";
    e.error = "Region is silent";
    REQUIRE(describeRow(e, 48000.0) == "Vox #2: Region is silent");
}

TEST_CASE("selection cap note", "[alignresults]")
{
    REQUIRE_FALSE(selectionIsCapped({ 0, 60 * 48000 }, 48000.0));
    REQUIRE(selectionIsCapped({ 0, 60 * 48000 + 1 }, 48000.0));
}

TEST_CASE("applyAlignments moves clips and skips missing ones", "[alignresults]")
{
    auto p = twoClips();
    REQUIRE(applyAlignments(p, { { "c1", 500 }, { "gone", 7 } }));
    REQUIRE(findClip(p, "c1")->start == 500);
    REQUIRE(findClip(p, "c0")->start == 0);
    int ti = -1;
    findClip(p, "c1", &ti);
    REQUIRE(ti == 1); // same track
    REQUIRE_FALSE(applyAlignments(p, { { "gone", 1 } }));
    REQUIRE_FALSE(applyAlignments(p, { { "c1", 500 } })); // no change
}

TEST_CASE("stale detection", "[alignresults]")
{
    auto p = twoClips(); // reference c0 on track 0, target c1 on track 1
    AlignRow r;
    r.clipId = "c1";
    r.anchor = captureAnchor(p, "c1");
    const auto ref = captureAnchor(p, "c0");
    REQUIRE(ref.exists);
    REQUIRE(ref.trackIndex == 0);
    REQUIRE(r.anchor.trackIndex == 1);

    SECTION("unchanged project is fresh")
    {
        REQUIRE_FALSE(isStale(p, ref, r));
        REQUIRE_FALSE(isStale(Project(p), ref, r));
    }
    SECTION("moved reference")
    {
        p.tracks[0].clips[0].start += 10;
        REQUIRE(isStale(p, ref, r));
    }
    SECTION("trimmed reference")
    {
        p.tracks[0].clips[0].sourceOffset += 5;
        p.tracks[0].clips[0].length -= 5;
        REQUIRE(isStale(p, ref, r));
    }
    SECTION("moved target")
    {
        p.tracks[1].clips[0].start += 1;
        REQUIRE(isStale(p, ref, r));
    }
    SECTION("target moved to another track")
    {
        auto c = p.tracks[1].clips[0];
        p.tracks[1].clips.clear();
        p.tracks[0].clips.push_back(c);
        REQUIRE(isStale(p, ref, r));
    }
    SECTION("unrelated clip moved or added")
    {
        Clip extra = p.tracks[0].clips[0];
        extra.id = "other";
        extra.start = 99999;
        p.tracks[1].clips.push_back(extra);
        p.tracks[1].clips.back().start += 7;
        REQUIRE_FALSE(isStale(p, ref, r));
    }
    SECTION("deleted target")
    {
        p.tracks[1].clips.clear();
        REQUIRE(isStale(p, ref, r));
    }
    SECTION("deleted reference")
    {
        p.tracks[0].clips.clear();
        REQUIRE(isStale(p, ref, r));
    }
    SECTION("missing at capture time is always stale")
    {
        AlignRow g;
        g.clipId = "nope";
        g.anchor = captureAnchor(p, "nope");
        REQUIRE_FALSE(g.anchor.exists);
        REQUIRE(isStale(p, ref, g));
    }
}
