#include "model/Grid.h"
#include <algorithm>
#include <cmath>

namespace aligna
{
double samplesPerBeat(const Project& p)
{
    return p.sampleRate * 60.0 / p.bpm * 4.0 / (double) p.beatUnit;
}

double gridStep(const Project& p, int subdivisionsPerBeat)
{
    return samplesPerBeat(p) / (double) std::max(1, subdivisionsPerBeat);
}

SamplePos snap(SamplePos pos, double step)
{
    if (! (step > 0.0))
        return pos;
    const long double s = (long double) step;
    const long double n = std::round((long double) pos / s);
    return (SamplePos) std::llround(n * s);
}

SamplePos nudgeAmount(NudgeSize size, const Project& p, int subdivisionsPerBeat)
{
    double v = 1.0;
    switch (size)
    {
        case NudgeSize::Sample: v = 1.0; break;
        case NudgeSize::OneMs: v = p.sampleRate / 1000.0; break;
        case NudgeSize::TenMs: v = p.sampleRate / 100.0; break;
        case NudgeSize::GridStep: v = gridStep(p, subdivisionsPerBeat); break;
    }
    return std::max<SamplePos>(1, (SamplePos) std::llround(v));
}

BarBeat toBarBeat(SamplePos pos, const Project& p)
{
    const double spb = samplesPerBeat(p);
    const int bpb = std::max(1, p.beatsPerBar);
    const long double beats = spb > 0 ? (long double) std::max<SamplePos>(0, pos) / (long double) spb : 0.0L;
    const long double whole = std::floor(beats);
    const long long wholeBeats = (long long) whole;
    BarBeat r;
    r.bar = (int) (wholeBeats / bpb) + 1;
    r.beat = (int) (wholeBeats % bpb) + 1;
    r.tick = (double) (beats - whole);
    return r;
}
} // namespace aligna

namespace aligna
{
bool setTimeSignature(Project& p, int beatsPerBar, int beatUnit)
{
    const bool unitOk = beatUnit == 1 || beatUnit == 2 || beatUnit == 4 || beatUnit == 8 || beatUnit == 16;
    if (beatsPerBar < 1 || beatsPerBar > 32 || ! unitOk)
        return false;
    if (p.beatsPerBar == beatsPerBar && p.beatUnit == beatUnit)
        return false;
    p.beatsPerBar = beatsPerBar;
    p.beatUnit = beatUnit;
    return true;
}
} // namespace aligna
