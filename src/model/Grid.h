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
} // namespace aligna
