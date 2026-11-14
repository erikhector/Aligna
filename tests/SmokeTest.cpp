#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

#include "core/Version.h"

TEST_CASE("smoke")
{
    REQUIRE(1 + 1 == 2);
}

TEST_CASE("core links juce and exposes version")
{
    REQUIRE(juce::String(aligna::versionString()) == "0.2.0");
}
