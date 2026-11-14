#include <catch2/catch_test_macros.hpp>
#include "edit/RecordTiming.h"

using namespace aligna;

TEST_CASE("prerollStart", "[record]") {
    Project p; p.sampleRate = 48000; p.bpm = 120;
    REQUIRE(prerollStart(p, 200000, true) == 104000);
    REQUIRE(prerollStart(p, 50000, true) == 0);      // clamped
    REQUIRE(prerollStart(p, 200000, false) == 200000);
    p.beatsPerBar = 3;
    REQUIRE(prerollStart(p, 200000, true) == 200000 - 72000);
}
TEST_CASE("totalLatency converts device to project samples", "[record]") {
    REQUIRE(totalLatency({ 256, 256, 0.0, 44100, 48000, 10.0 }) == 1037); // 512*48000/44100 + 480 = 1037.28
    REQUIRE(totalLatency({ 0, 0, 0.0, 48000, 48000, -500.0 }) == -24000);
}
TEST_CASE("placeTake", "[record]") {
    auto a = placeTake(104000, 200000, 480, 500000, 48000);
    REQUIRE(a); REQUIRE(a->start == 200000); REQUIRE(a->sourceOffset == 96480); REQUIRE(a->length == 403520);
    auto b = placeTake(200000, 200000, -480, 100000, 48000);          // negative offset, pre-roll off
    REQUIRE(b); REQUIRE(b->start == 200480); REQUIRE(b->sourceOffset == 0); REQUIRE(b->length == 100000);
    auto c = placeTake(104000, 200000, -120000, 500000, 48000);       // offset bigger than pre-roll
    REQUIRE(c); REQUIRE(c->start == 224000); REQUIRE(c->sourceOffset == 0); REQUIRE(c->length == 500000);
    REQUIRE_FALSE(placeTake(200000, 200000, 0, 4799, 48000));
    REQUIRE(placeTake(200000, 200000, 0, 4800, 48000));
}
TEST_CASE("nextTakeName", "[record]") {
    Project p;
    REQUIRE(nextTakeName(p) == "Take 1");
    for (auto n : { "Vocals", "Take 1", "Take 3", "Take x", "Take 2b" }) { Track t; t.name = n; p.tracks.push_back(t); }
    REQUIRE(nextTakeName(p) == "Take 4");
}
TEST_CASE("clampLatencyOffsetMs", "[record]") {
    REQUIRE(clampLatencyOffsetMs(-900) == -500.0); REQUIRE(clampLatencyOffsetMs(12.5) == 12.5); REQUIRE(clampLatencyOffsetMs(700) == 500.0);
}
