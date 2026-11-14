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

TEST_CASE("toCoordClamped clamps without rounding")
{
    REQUIRE(toCoordClamped(1.0e15, 1000) == Catch::Approx(1016.0));
    REQUIRE(toCoordClamped(-1.0e15, 1000) == Catch::Approx(-16.0));
    REQUIRE(toCoordClamped(std::nan(""), 1000) == Catch::Approx(0.0));
    REQUIRE(toCoordClamped(250.25, 1000) == Catch::Approx(250.25));

    // A long selection at max zoom: both edges stay far below the renderer's fixed-point limit.
    TimelineMapping m;
    m.samplesPerPixel = TimelineMapping::minSamplesPerPixel;
    m.viewStart = 48000 * 60;
    const float a = toCoordClamped(m.toX(0), 1200), b = toCoordClamped(m.toX(48000 * 600), 1200);
    REQUIRE(a == Catch::Approx(-16.0));
    REQUIRE(b == Catch::Approx(1216.0));
}

TEST_CASE("clipLineToWidth cuts a line to the view and keeps its slope")
{
    juce::Line<float> l;

    // Entirely in range: unchanged.
    REQUIRE(clipLineToWidth(10.0, 100.0, 60.0, 0.0, 1000, l));
    REQUIRE(l.getStartX() == Catch::Approx(10.0));
    REQUIRE(l.getStartY() == Catch::Approx(100.0));
    REQUIRE(l.getEndX() == Catch::Approx(60.0));
    REQUIRE(l.getEndY() == Catch::Approx(0.0));

    // A fade-in that started far to the left: cut at -16, at the height the full line has there.
    REQUIRE(clipLineToWidth(-1.0e9, 100.0, 500.0, 0.0, 1000, l));
    REQUIRE(l.getStartX() == Catch::Approx(-16.0));
    REQUIRE(l.getStartY() == Catch::Approx(100.0 * 516.0 / (1.0e9 + 500.0)).margin(1.0e-3));
    REQUIRE(l.getEndX() == Catch::Approx(500.0));
    REQUIRE(l.getEndY() == Catch::Approx(0.0));

    // Both ends far outside, crossing the view: a nearly flat piece in the middle of the slope.
    REQUIRE(clipLineToWidth(-1.0e9, 0.0, 1.0e9, 100.0, 1000, l));
    REQUIRE(l.getStartX() == Catch::Approx(-16.0));
    REQUIRE(l.getEndX() == Catch::Approx(1016.0));
    REQUIRE(l.getStartY() == Catch::Approx(50.0).margin(1.0e-3));
    REQUIRE(l.getEndY() == Catch::Approx(50.0).margin(1.0e-3));

    // Entirely off one side, or not a number: nothing to draw.
    REQUIRE_FALSE(clipLineToWidth(-5000.0, 0.0, -100.0, 100.0, 1000, l));
    REQUIRE_FALSE(clipLineToWidth(2000.0, 0.0, 1.0e12, 100.0, 1000, l));
    REQUIRE_FALSE(clipLineToWidth(std::nan(""), 0.0, 10.0, 100.0, 1000, l));

    // Vertical line in range.
    REQUIRE(clipLineToWidth(40.0, 0.0, 40.0, 100.0, 1000, l));
    REQUIRE(l.getStartX() == Catch::Approx(40.0));
    REQUIRE(l.getEndY() == Catch::Approx(100.0));
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

TEST_CASE("hitTest fade handles are inactive for clips 36 px wide or narrower")
{
    Project p;
    Track t;
    Clip a; a.id = "a"; a.start = 0; a.length = 512 * 36; a.fadeIn = 512 * 10; // exactly 36 px wide
    Clip b; b.id = "b"; b.start = 512 * 100; b.length = 512 * 37; b.fadeIn = 512 * 10; // 37 px wide
    t.clips = { a, b };
    p.tracks.push_back(t);
    TimelineMapping m;
    auto h = hitTest(p, m, 100, { 10.0f, 5.0f });
    REQUIRE(h.clipId == "a");
    REQUIRE(h.zone == HitZone::Body);
    h = hitTest(p, m, 100, { 2.0f, 5.0f });
    REQUIRE(h.zone == HitZone::TrimStart);
    h = hitTest(p, m, 100, { 110.0f, 5.0f }); // 37 px wide: fade handle at x=110 is active
    REQUIRE(h.clipId == "b");
    REQUIRE(h.zone == HitZone::FadeIn);
}
