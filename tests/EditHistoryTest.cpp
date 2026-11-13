#include "edit/EditHistory.h"
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
        findClip(p, id)->start = 100;
    }
};
SamplePos startOf(const Project& p, const juce::String& id)
{
    Project copy = p;
    return findClip(copy, id)->start;
}
auto bump = [](Project& p) { p.bpm += 1; return true; };
} // namespace

TEST_CASE("EditHistory undo and redo a move", "[EditHistory]")
{
    Fx f;
    EditHistory h(f.p);
    int changes = 0;
    h.onChange = [&] { ++changes; };
    REQUIRE_FALSE(h.canUndo());
    REQUIRE(h.perform("Move", [&](Project& p) { return moveClip(p, f.id, 500, 0); }));
    CHECK(startOf(h.current(), f.id) == 500);
    CHECK(h.undoName() == "Move");
    CHECK(h.undo());
    CHECK(startOf(h.current(), f.id) == 100);
    CHECK(h.redoName() == "Move");
    CHECK(h.redo());
    CHECK(startOf(h.current(), f.id) == 500);
    CHECK(changes == 3);
    CHECK_FALSE(h.redo());
    CHECK(changes == 3);
}

TEST_CASE("EditHistory failing edit leaves no trace", "[EditHistory]")
{
    Fx f;
    EditHistory h(f.p);
    int changes = 0;
    h.onChange = [&] { ++changes; };
    CHECK_FALSE(h.perform("Bad", [](Project& p) { p.bpm = 999; return false; }));
    CHECK_FALSE(h.canUndo());
    CHECK(h.current().bpm < 121.0);
    CHECK(changes == 0);
    CHECK_FALSE(h.undo());
    CHECK(changes == 0);
}

TEST_CASE("EditHistory perform clears redo", "[EditHistory]")
{
    EditHistory h{ Project{} };
    h.perform("a", bump);
    h.undo();
    REQUIRE(h.canRedo());
    h.perform("b", bump);
    CHECK_FALSE(h.canRedo());
}

TEST_CASE("EditHistory keeps 200 steps", "[EditHistory]")
{
    EditHistory h{ Project{} };
    for (int i = 0; i < 205; ++i)
        h.perform("s", bump);
    int n = 0;
    while (h.undo())
        ++n;
    CHECK(n == 200);
    CHECK((h.current().bpm > 324.0 && h.current().bpm < 326.0));
}

TEST_CASE("EditHistory dirty tracking", "[EditHistory]")
{
    EditHistory h{ Project{} };
    CHECK_FALSE(h.isDirty());
    h.markSaved();
    h.perform("a", bump);
    CHECK(h.isDirty());
    h.undo();
    CHECK_FALSE(h.isDirty());
    h.redo();
    CHECK(h.isDirty());
    h.markSaved();
    CHECK_FALSE(h.isDirty());
    h.undo();
    CHECK(h.isDirty());
    h.perform("b", bump); // saved state discarded
    h.undo();
    CHECK(h.isDirty());
    h.redo();
    CHECK(h.isDirty());
}

TEST_CASE("EditHistory dirty when saved state dropped from window", "[EditHistory]")
{
    EditHistory h{ Project{} };
    h.markSaved();
    for (int i = 0; i < 201; ++i)
        h.perform("s", bump);
    while (h.undo()) {}
    CHECK(h.isDirty());
}

TEST_CASE("EditHistory reset clears and is clean", "[EditHistory]")
{
    EditHistory h{ Project{} };
    int changes = 0;
    h.onChange = [&] { ++changes; };
    h.perform("a", bump);
    Project q;
    q.bpm = 90;
    h.reset(q);
    CHECK(changes == 2);
    CHECK_FALSE(h.canUndo());
    CHECK_FALSE(h.canRedo());
    CHECK_FALSE(h.isDirty());
    CHECK((h.current().bpm > 89.0 && h.current().bpm < 91.0));
}
