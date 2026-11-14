#pragma once
#include "model/Project.h"
#include <optional>

namespace aligna
{
struct TakePlacement
{
    SamplePos start = 0, sourceOffset = 0, length = 0;
};

constexpr double kMinLatencyOffsetMs = -500.0, kMaxLatencyOffsetMs = 500.0;
double clampLatencyOffsetMs(double ms);

// One bar before recordStart when pre-roll is on (clamped at 0), else recordStart.
SamplePos prerollStart(const Project&, SamplePos recordStart, bool prerollOn);

struct LatencyInputs
{
    int inputLatency = 0, outputLatency = 0;
    double chainDelay = 0; // device samples
    double deviceRate = 48000, projectRate = 48000, manualOffsetMs = 0;
};
// Total latency in project samples, rounded to nearest. May be negative.
SamplePos totalLatency(const LatencyInputs&);

// nullopt if the take is shorter than 0.1 s.
std::optional<TakePlacement> placeTake(SamplePos prerollStart, SamplePos recordStart, SamplePos latency,
                                       SamplePos recordedLength, double projectRate);

// "Take N", N = 1 + highest existing track named "Take <integer>".
juce::String nextTakeName(const Project&);
} // namespace aligna
