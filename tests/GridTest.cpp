#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "model/Grid.h"

using namespace aligna;
using Catch::Approx;

TEST_CASE("grid basics at 48k/120/4-4")
{
    Project p;
    REQUIRE(samplesPerBeat(p) == Approx(24000.0));
    REQUIRE(gridStep(p, 4) == Approx(6000.0));
}

TEST_CASE("6/8 samplesPerBeat")
{
    Project p;
    p.beatsPerBar = 6;
    p.beatUnit = 8;
    REQUIRE(samplesPerBeat(p) == Approx(12000.0));
}

TEST_CASE("snap rounds to nearest multiple")
{
    REQUIRE(snap(8999, 6000) == 6000);
    REQUIRE(snap(9001, 6000) == 12000);
    REQUIRE(snap(-9001, 6000) == -12000);
    REQUIRE(snap(1234, 0) == 1234);
    REQUIRE(snap(1234, -5) == 1234);
    REQUIRE(snap(3'000'000'001, 6000) == 3'000'000'000);
}

TEST_CASE("nudge amounts")
{
    Project p;
    REQUIRE(nudgeAmount(NudgeSize::Sample, p, 4) == 1);
    REQUIRE(nudgeAmount(NudgeSize::OneMs, p, 4) == 48);
    REQUIRE(nudgeAmount(NudgeSize::TenMs, p, 4) == 480);
    REQUIRE(nudgeAmount(NudgeSize::GridStep, p, 4) == 6000);
    p.bpm = 1e9; // absurdly tiny grid: minimum 1
    REQUIRE(nudgeAmount(NudgeSize::GridStep, p, 4) == 1);
}

TEST_CASE("toBarBeat")
{
    Project p;
    auto a = toBarBeat(96000, p);
    REQUIRE(a.bar == 2);
    REQUIRE(a.beat == 1);
    REQUIRE(a.tick == Approx(0.0));
    auto b = toBarBeat(24000 * 5 + 12000, p);
    REQUIRE(b.bar == 2);
    REQUIRE(b.beat == 2);
    REQUIRE(b.tick == Approx(0.5));
    auto c = toBarBeat(-100, p);
    REQUIRE(c.bar == 1);
    REQUIRE(c.beat == 1);
    REQUIRE(c.tick == Approx(0.0));
}

TEST_CASE("setTimeSignature validates and reports change")
{
    Project p;
    REQUIRE(setTimeSignature(p, 3, 4));
    REQUIRE(p.beatsPerBar == 3);
    REQUIRE(p.beatUnit == 4);
    REQUIRE_FALSE(setTimeSignature(p, 3, 4)); // unchanged
    REQUIRE(setTimeSignature(p, 7, 8));
    REQUIRE(p.beatsPerBar == 7);
    REQUIRE(p.beatUnit == 8);
    for (int unit : { 1, 2, 4, 8, 16 })
        REQUIRE(setTimeSignature(p, 2, unit));
    REQUIRE(setTimeSignature(p, 32, 16));
    REQUIRE(setTimeSignature(p, 1, 1));
    // Invalid input leaves the project untouched.
    REQUIRE_FALSE(setTimeSignature(p, 0, 4));
    REQUIRE_FALSE(setTimeSignature(p, 33, 4));
    REQUIRE_FALSE(setTimeSignature(p, 4, 3));
    REQUIRE_FALSE(setTimeSignature(p, 4, 32));
    REQUIRE_FALSE(setTimeSignature(p, 4, 0));
    REQUIRE(p.beatsPerBar == 1);
    REQUIRE(p.beatUnit == 1);
}
