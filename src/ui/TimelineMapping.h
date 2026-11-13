#pragma once
#include "model/Project.h"
#include <juce_graphics/juce_graphics.h>

namespace aligna
{
struct TimelineMapping
{
    static constexpr double minSamplesPerPixel = 1.0 / 16.0;
    static constexpr double maxSamplesPerPixel = (double) (1 << 20);

    double samplesPerPixel = 512;
    SamplePos viewStart = 0;

    double toX(SamplePos s) const { return (double) (s - viewStart) / samplesPerPixel; }
    SamplePos toSample(double x) const { return viewStart + (SamplePos) std::llround(x * samplesPerPixel); }
    // Keeps the sample under x fixed; clamps zoom and viewStart >= 0.
    void zoomAround(double x, double factor);
};

enum class HitZone { None, Body, TrimStart, TrimEnd, FadeIn, FadeOut };
struct Hit
{
    juce::String clipId;
    int trackIndex = -1;
    HitZone zone = HitZone::None;
};

// p is relative to the top-left of the first lane (x in timeline pixels).
Hit hitTest(const Project&, const TimelineMapping&, int trackHeight, juce::Point<float> p);
} // namespace aligna
