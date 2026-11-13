#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "ui/TimelineMapping.h"

using namespace aligna;

TEST_CASE("mapping round-trips samples")
{
    for (double spp : { 0.25, 512.0 })
        for (SamplePos s : { (SamplePos) 0, (SamplePos) 12345, (SamplePos) 3'000'000'000 })
        {
            TimelineMapping m;
            m.samplesPerPixel = spp;
            REQUIRE(m.toSample(m.toX(s)) == s);
        }
}

TEST_CASE("mapping respects viewStart")
{
    TimelineMapping m;
    m.viewStart = 1024;
    REQUIRE(m.toX(1024) == Catch::Approx(0.0));
    REQUIRE(m.toX(1536) == Catch::Approx(1.0));
}

TEST_CASE("zoomAround keeps the sample under the cursor")
{
    TimelineMapping m;
    m.samplesPerPixel = 512;
    m.viewStart = 100000;
    const auto before = m.toSample(300);
    m.zoomAround(300, 0.5);
    REQUIRE(m.samplesPerPixel == Catch::Approx(256.0));
    REQUIRE(std::llabs(m.toSample(300) - before) <= 1);
}

TEST_CASE("zoomAround clamps zoom and viewStart")
{
    TimelineMapping m;
    for (int i = 0; i < 100; ++i) m.zoomAround(50, 0.5);
    REQUIRE(m.samplesPerPixel == Catch::Approx(1.0 / 16.0));
    for (int i = 0; i < 100; ++i) m.zoomAround(50, 2.0);
    REQUIRE(m.samplesPerPixel == Catch::Approx((double) (1 << 20)));
    REQUIRE(m.viewStart >= 0);
}

namespace
{
Project twoClips()
{
    Project p;
    Track t;
    Clip a; a.id = "a"; a.start = 0; a.length = 51200;      // 0..100 px at 512
    Clip b; b.id = "b"; b.start = 25600; b.length = 51200;   // 50..150 px
    t.clips = { a, b };
    p.tracks.push_back(t);
    return p;
}
}

TEST_CASE("hitTest zones")
{
    auto p = twoClips();
    TimelineMapping m;
    // Clip a spans x 0..100; click 3 px inside left edge of b (x=53), mid-height.
    auto h = hitTest(p, m, 100, { 53.0f, 50.0f });
    REQUIRE(h.clipId == "b");
    REQUIRE(h.zone == HitZone::TrimStart);
    REQUIRE(h.trackIndex == 0);

    h = hitTest(p, m, 100, { 120.0f, 50.0f });
    REQUIRE(h.clipId == "b");
    REQUIRE(h.zone == HitZone::Body);

    h = hitTest(p, m, 100, { 147.0f, 50.0f });
    REQUIRE(h.zone == HitZone::TrimEnd);

    h = hitTest(p, m, 100, { 20.0f, 50.0f });
    REQUIRE(h.clipId == "a");
    REQUIRE(h.zone == HitZone::Body);
}

TEST_CASE("hitTest later clip wins in overlap")
{
    auto p = twoClips();
    TimelineMapping m;
    auto h = hitTest(p, m, 100, { 75.0f, 50.0f });
    REQUIRE(h.clipId == "b");
}

TEST_CASE("hitTest fade handles and misses")
{
    auto p = twoClips();
    p.tracks[0].clips[1].fadeIn = 12800;  // handle at x = 75
    TimelineMapping m;
    auto h = hitTest(p, m, 100, { 78.0f, 5.0f });
    REQUIRE(h.zone == HitZone::FadeIn);
    h = hitTest(p, m, 100, { 78.0f, 50.0f });
    REQUIRE(h.zone == HitZone::Body);

    h = hitTest(p, m, 100, { 500.0f, 50.0f });
    REQUIRE(h.clipId.isEmpty());
    REQUIRE(h.zone == HitZone::None);
    REQUIRE(h.trackIndex == 0);
    h = hitTest(p, m, 100, { 20.0f, 250.0f });
    REQUIRE(h.trackIndex == -1);
}

TEST_CASE("toPixelClamped clamps far-off-screen coordinates")
{
    REQUIRE(toPixelClamped(1.0e15, 1000) == 1016);
    REQUIRE(toPixelClamped(-1.0e15, 1000) == -16);
    REQUIRE(toPixelClamped(std::nan(""), 1000) == 0);
    REQUIRE(toPixelClamped(250.4, 1000) == 250);
}

TEST_CASE("a 3e9-sample clip at max zoom gives a sane on-screen rect")
{
    TimelineMapping m;
    m.samplesPerPixel = TimelineMapping::minSamplesPerPixel;
    m.viewStart = 1000;
    const int l = toPixelClamped(m.toX(0), 1200), r = toPixelClamped(m.toX(3'000'000'000), 1200);
    REQUIRE(l == -16);
    REQUIRE(r == 1216);
    REQUIRE(r - l == 1232);
}
