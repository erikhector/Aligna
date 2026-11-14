#include "edit/ClipOps.h"
#include "edit/EditHistory.h"
#include "engine/StretchCache.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace aligna;
using Catch::Approx;

namespace
{
struct Fx
{
    Project p;
    juce::String id;
    explicit Fx(SamplePos srcLen = 48000)
    {
        p.bpm = 100;
        SourceInfo s;
        s.id = "src";
        s.path = "a.wav";
        s.length = srcLen;
        s.numChannels = 1;
        id = addTrackWithClip(p, s, "T1");
        c().start = 1000;
    }
    Clip& c() { return *findClip(p, id); }
};
} // namespace

TEST_CASE("setClipStretch scales length and keeps start", "[stretch][ClipOps]")
{
    Fx f;
    CHECK(setClipStretch(f.p, f.id, 2.0));
    CHECK(f.c().length == 96000);
    CHECK(f.c().start == 1000);
    CHECK(f.c().stretch == Approx(2.0));
    CHECK(bufferKey(f.c()) != f.c().sourceId);
    CHECK_FALSE(setClipStretch(f.p, f.id, 2.0));
    CHECK(setClipStretch(f.p, f.id, 1.0));
    CHECK(bufferKey(f.c()) == f.c().sourceId);
}

TEST_CASE("setClipStretch clamps", "[stretch][ClipOps]")
{
    Fx f;
    CHECK(setClipStretch(f.p, f.id, 10.0));
    CHECK(f.c().stretch == Approx(4.0));
    CHECK(f.c().length == 192000);
    CHECK_FALSE(setClipStretch(f.p, f.id, 10.0));
    CHECK(setClipStretch(f.p, f.id, 0.01));
    CHECK(f.c().stretch == Approx(0.25));
    CHECK_FALSE(setClipStretch(f.p, "nope", 2.0));
}

TEST_CASE("setClipStretch on a trimmed clip scales offset", "[stretch][ClipOps]")
{
    Fx f;
    f.c().sourceOffset = 12000;
    f.c().length = 24000;
    CHECK(setClipStretch(f.p, f.id, 2.0));
    CHECK(f.c().sourceOffset == 24000);
    CHECK(f.c().length == 48000);
}

TEST_CASE("setClipStretch scales fades", "[stretch][ClipOps]")
{
    Fx f;
    f.c().fadeIn = 4800;
    f.c().fadeOut = 4800;
    CHECK(setClipStretch(f.p, f.id, 0.5));
    CHECK(f.c().fadeIn == 2400);
    CHECK(f.c().fadeOut == 2400);
}

TEST_CASE("setClipStretch clears sourceBpm", "[stretch][ClipOps]")
{
    Fx f;
    setClipTempo(f.p, f.id, 98);
    CHECK(f.c().sourceBpm == Approx(98));
    CHECK(setClipStretch(f.p, f.id, 1.5));
    CHECK(f.c().sourceBpm == Approx(0));
}

TEST_CASE("trimClipEnd clamps to the stretched source", "[stretch][ClipOps]")
{
    Fx f;
    setClipStretch(f.p, f.id, 2.0);
    f.c().length = 1000;
    f.c().sourceOffset = 10000;
    trimClipEnd(f.p, f.id, 100'000'000);
    CHECK(f.c().length == stretchedSourceLength(f.p, f.c()) - 10000);
    CHECK(f.c().length == 96000 - 10000);
}

TEST_CASE("addTrackWithClip uses source length", "[stretch][ClipOps]")
{
    Fx f(12345);
    CHECK(f.c().length == 12345);
    CHECK(f.c().stretch == Approx(1.0));
}

TEST_CASE("splitClip keeps stretch and sourceBpm", "[stretch][ClipOps]")
{
    Fx f;
    setClipTempo(f.p, f.id, 50); // stretch 0.5
    auto right = splitClip(f.p, f.id, 1000 + 10000);
    REQUIRE(right.has_value());
    CHECK(f.c().stretch == Approx(0.5));
    CHECK(f.c().sourceBpm == Approx(50));
    auto* r = findClip(f.p, *right);
    REQUIRE(r != nullptr);
    CHECK(r->stretch == Approx(0.5));
    CHECK(r->sourceBpm == Approx(50));
}

TEST_CASE("setClipTempo", "[stretch][ClipOps]")
{
    Fx f;
    CHECK(setClipTempo(f.p, f.id, 98));
    CHECK(f.c().stretch == Approx(0.98));
    CHECK(f.c().sourceBpm == Approx(98));
    CHECK(f.c().length == std::llround(48000 * 0.98));
    CHECK_FALSE(setClipTempo(f.p, f.id, 98));
    CHECK(setClipTempo(f.p, f.id, 0));
    CHECK(f.c().sourceBpm == Approx(0));
    CHECK(f.c().stretch == Approx(0.98));
    CHECK_FALSE(setClipTempo(f.p, f.id, 0));
}

TEST_CASE("setProjectBpm restretches tempo clips only", "[stretch][ClipOps]")
{
    Fx f;
    setClipTempo(f.p, f.id, 98);
    // second, manually stretched clip
    SourceInfo s;
    s.id = "src";
    s.length = 48000;
    auto id2 = addTrackWithClip(f.p, s, "T2");
    setClipStretch(f.p, id2, 1.5);
    findClip(f.p, id2)->start = 5000;
    const auto len2 = findClip(f.p, id2)->length;

    CHECK(setProjectBpm(f.p, 196));
    CHECK(f.p.bpm == Approx(196));
    CHECK(f.c().stretch == Approx(0.5));
    CHECK(f.c().length == 24000);
    CHECK(f.c().start == 1000);
    CHECK(findClip(f.p, id2)->stretch == Approx(1.5));
    CHECK(findClip(f.p, id2)->length == len2);
    CHECK(findClip(f.p, id2)->start == 5000);
    CHECK_FALSE(setProjectBpm(f.p, 196));
}

TEST_CASE("setProjectBpm undo restores everything", "[stretch][ClipOps]")
{
    Fx f;
    setClipTempo(f.p, f.id, 98);
    const double st = f.c().stretch;
    const auto len = f.c().length;
    EditHistory h(f.p);
    REQUIRE(h.perform("Tempo", [](Project& p) { return setProjectBpm(p, 196); }));
    CHECK(h.current().tracks[0].clips[0].stretch == Approx(0.5));
    REQUIRE(h.undo());
    CHECK(h.current().bpm == Approx(100));
    CHECK(h.current().tracks[0].clips[0].stretch == Approx(st));
    CHECK(h.current().tracks[0].clips[0].length == len);
}

TEST_CASE("maxStretchFor caps at two hours", "[stretch][ClipOps]")
{
    Fx f(3600LL * 48000);
    CHECK(maxStretchFor(f.p, f.c()) == Approx(2.0));
    Fx g;
    CHECK(maxStretchFor(g.p, g.c()) == Approx(4.0));
    CHECK(setClipStretch(f.p, f.id, 4.0));
    CHECK(f.c().stretch == Approx(2.0));
}

TEST_CASE("clipSourceRange follows trims, splits and a new ratio", "[stretch][ClipOps]")
{
    // What the stretch cache renders is derived from this range, so it has to track every edit of a stretched clip.
    const SamplePos srcLen = 10 * 48000;
    Fx f(srcLen);
    REQUIRE(setClipStretch(f.p, f.id, 2.0));
    CHECK(clipSourceRange(f.c(), srcLen) == juce::Range<SamplePos>(0, srcLen)); // the whole source

    // Timeline (stretched) samples map to source samples through the ratio: 2 s trimmed off is 1 s of source.
    REQUIRE(trimClipStart(f.p, f.id, 1000 + 2 * 48000));
    REQUIRE(trimClipEnd(f.p, f.id, 1000 + 12 * 48000));
    CHECK(f.c().sourceOffset == 2 * 48000);
    CHECK(clipSourceRange(f.c(), srcLen) == juce::Range<SamplePos>(1 * 48000, 6 * 48000));

    // Moving the clip changes nothing.
    REQUIRE(moveClip(f.p, f.id, 777777, 0));
    CHECK(clipSourceRange(f.c(), srcLen) == juce::Range<SamplePos>(1 * 48000, 6 * 48000));

    // The halves of a split adjoin in the source (an odd split point is shared: both round outwards).
    for (SamplePos d : { SamplePos(96000), SamplePos(96001) })
    {
        CAPTURE(d);
        auto p = f.p;
        const auto before = clipSourceRange(*findClip(p, f.id), srcLen);
        auto rightId = splitClip(p, f.id, findClip(p, f.id)->start + d);
        REQUIRE(rightId.has_value());
        const auto left = clipSourceRange(*findClip(p, f.id), srcLen);
        const auto right = clipSourceRange(*findClip(p, *rightId), srcLen);
        CHECK(left.getStart() == before.getStart());
        CHECK(right.getEnd() == before.getEnd());
        CHECK(left.getEnd() - right.getStart() == d % 2);
        CHECK(left.getUnionWith(right) == before);
    }

    // A new ratio rescales offset and length: the same source audio, give or take the rounding.
    REQUIRE(setClipStretch(f.p, f.id, 1.5));
    const auto r = clipSourceRange(f.c(), srcLen);
    CHECK(std::abs(r.getStart() - 1 * 48000) <= 1);
    CHECK(std::abs(r.getEnd() - 6 * 48000) <= 1);
}
