#pragma once
#include "model/Project.h"

namespace aligna
{
double samplesPerBeat(const Project&);
double gridStep(const Project&, int subdivisionsPerBeat);
// Nearest multiple of step; step <= 0 returns pos unchanged.
SamplePos snap(SamplePos pos, double step);

enum class NudgeSize { Sample, OneMs, TenMs, GridStep };
SamplePos nudgeAmount(NudgeSize, const Project&, int subdivisionsPerBeat);

struct BarBeat { int bar, beat; double tick; }; // bar/beat 1-based, tick in [0,1)
BarBeat toBarBeat(SamplePos, const Project&);

// Sets the time signature. Returns false (project untouched) if beatsPerBar is outside 1..32,
// beatUnit is not 1/2/4/8/16, or the signature is unchanged.
bool setTimeSignature(Project&, int beatsPerBar, int beatUnit);
} // namespace aligna
