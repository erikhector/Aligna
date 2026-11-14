#include "align/Correlate.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>

namespace aligna
{
namespace
{
const char* const kCancelled = "Cancelled";
const char* const kSilent = "Clip is silent in the compared region";
const char* const kInvalid = "Clip contains invalid audio in the compared region";
constexpr double kSilenceRms = 1e-5;
constexpr float kNoSecondPeakConfidence = 1000.0f;

// Loop iterations between two cancellation checks in the long passes (well under a millisecond of work each).
constexpr SamplePos kCheckBlock = 1 << 16;

// Runs body(b, e) over [begin, end) in blocks of kCheckBlock, calling check(fraction done) before each block;
// returns false as soon as check does. Blocks are visited in order, so anything body accumulates is unchanged.
template <typename Check, typename Body>
bool inBlocks(SamplePos begin, SamplePos end, Check&& check, Body&& body)
{
    for (SamplePos b = begin; b < end; b += kCheckBlock)
    {
        if (! check((float) (b - begin) / (float) (end - begin)))
            return false;
        body(b, std::min(end, b + kCheckBlock));
    }
    return true;
}

// nullopt when check cancels.
template <typename Check>
std::optional<double> rms(const std::vector<float>& v, Check&& check)
{
    if (v.empty())
        return 0.0;
    double s = 0.0;
    if (! inBlocks(0, (SamplePos) v.size(), check, [&](SamplePos b, SamplePos e) {
            for (auto i = (size_t) b; i < (size_t) e; ++i)
                s += (double) v[i] * (double) v[i];
        }))
        return std::nullopt;
    return std::sqrt(s / (double) v.size());
}

// Moving average of length d, then keep every d-th sample. nullopt when check cancels.
template <typename Check>
std::optional<std::vector<float>> decimate(const std::vector<float>& x, int d, Check&& check)
{
    if (d <= 1)
        return x;
    std::vector<float> out((x.size() + (size_t) d - 1) / (size_t) d);
    if (! inBlocks(0, (SamplePos) out.size(), check, [&](SamplePos kb, SamplePos ke) {
            for (auto k = (size_t) kb; k < (size_t) ke; ++k)
            {
                double s = 0.0;
                const size_t b = k * (size_t) d, e = std::min(x.size(), b + (size_t) d);
                for (size_t i = b; i < e; ++i)
                    s += x[i];
                out[k] = (float) (s / d);
            }
        }))
        return std::nullopt;
    return out;
}

// Floor division for possibly negative numerators (d > 0).
SamplePos floorDiv(SamplePos a, SamplePos d) { return a >= 0 ? a / d : -((-a + d - 1) / d); }

int ceilLog2(size_t n)
{
    int order = 0;
    while (((size_t) 1 << order) < n)
        ++order;
    return order;
}

// Normalised cross-correlation at index lag l: sum t[n] r[n-l] / sqrt(sum t^2 * sum r^2) over the overlap.
double normalisedCorrelation(const std::vector<float>& r, const std::vector<float>& t, SamplePos l)
{
    const SamplePos nBegin = std::max<SamplePos>(0, l);
    const SamplePos nEnd = std::min<SamplePos>((SamplePos) t.size(), (SamplePos) r.size() + l);
    double xy = 0.0, xx = 0.0, yy = 0.0;
    for (SamplePos n = nBegin; n < nEnd; ++n)
    {
        const double a = t[(size_t) n], b = r[(size_t) (n - l)];
        xy += a * b;
        xx += a * a;
        yy += b * b;
    }
    const double den = std::sqrt(xx * yy);
    return den > 0.0 ? xy / den : 0.0;
}
} // namespace

std::optional<OffsetResult> findOffset(const std::vector<float>& reference, const std::vector<float>& target,
                                       SamplePos targetIndexOffset, SamplePos maxLag, double sampleRate,
                                       const std::function<bool(float)>& progress, juce::String& error)
{
    auto report = [&](float p) { return !progress || progress(p); };
    // Cancellation check for a long loop that takes the overall progress from `from` to `to`.
    auto stage = [&](float from, float to) {
        return [&report, from, to](float fraction) { return report(from + (to - from) * fraction); };
    };
    auto cancelled = [&]() -> std::optional<OffsetResult> {
        error = kCancelled;
        return std::nullopt;
    };

    if (!report(0.0f))
        return cancelled();

    const auto refRmsDone = rms(reference, stage(0.0f, 0.01f));
    if (! refRmsDone)
        return cancelled();
    const auto tgtRmsDone = rms(target, stage(0.01f, 0.02f));
    if (! tgtRmsDone)
        return cancelled();
    const double refRms = *refRmsDone, tgtRms = *tgtRmsDone;
    if (! std::isfinite(refRms) || ! std::isfinite(tgtRms)) // NaN/Inf samples: nothing below would be meaningful
    {
        error = kInvalid;
        return std::nullopt;
    }
    if (! (refRms >= kSilenceRms) || ! (tgtRms >= kSilenceRms))
    {
        error = kSilent;
        return std::nullopt;
    }

    maxLag = std::max<SamplePos>(0, maxLag);
    // Index lag l (target[i] ≈ ±reference[i - l]) relates to the reference-position lag L by L = l + offset.
    const SamplePos loIdx = -maxLag - targetIndexOffset, hiIdx = maxLag - targetIndexOffset;

    // ---- Coarse pass: GCC-PHAT at ~8 kHz -------------------------------------------------------------
    const int d = std::max(1, (int) std::floor(sampleRate / 8000.0));
    const auto refDecimated = decimate(reference, d, stage(0.02f, 0.03f));
    if (! refDecimated)
        return cancelled();
    const auto tgtDecimated = decimate(target, d, stage(0.03f, 0.05f));
    if (! tgtDecimated)
        return cancelled();
    const auto& refD = *refDecimated;
    const auto& tgtD = *tgtDecimated;
    if (!report(0.05f))
        return cancelled();

    const int order = ceilLog2(refD.size() + tgtD.size());
    const size_t N = (size_t) 1 << order;
    // Setting up a large transform and each of its buffers takes a while too, hence the checks in between.
    juce::dsp::FFT fft(order);
    if (!report(0.05f))
        return cancelled();

    std::vector<float> X(2 * N, 0.0f);
    std::copy(tgtD.begin(), tgtD.end(), X.begin());
    if (!report(0.05f))
        return cancelled();
    std::vector<float> Y(2 * N, 0.0f);
    std::copy(refD.begin(), refD.end(), Y.begin());
    if (!report(0.05f))
        return cancelled();
    // One transform cannot be interrupted; there is a check between every two of them.
    fft.performRealOnlyForwardTransform(X.data(), true);
    if (!report(0.2f))
        return cancelled();
    fft.performRealOnlyForwardTransform(Y.data(), true);
    if (!report(0.35f))
        return cancelled();

    // R = X·conj(Y) / |X·conj(Y)|; its inverse peaks at the index lag (in decimated samples).
    if (! inBlocks(0, (SamplePos) (N / 2) + 1, stage(0.35f, 0.45f), [&](SamplePos kb, SamplePos ke) {
            for (auto k = (size_t) kb; k < (size_t) ke; ++k)
            {
                const std::complex<float> x(X[2 * k], X[2 * k + 1]), y(Y[2 * k], Y[2 * k + 1]);
                const auto c = x * std::conj(y);
                const auto r = c / std::max(std::abs(c), 1e-12f);
                X[2 * k] = r.real();
                X[2 * k + 1] = r.imag();
            }
        }))
        return cancelled();
    for (size_t k = N / 2 + 1; k < N; ++k)
        X[2 * k] = X[2 * k + 1] = 0.0f;
    if (!report(0.45f))
        return cancelled();

    fft.performRealOnlyInverseTransform(X.data());
    if (!report(0.6f))
        return cancelled();

    // Search the coarse lags covering [loIdx, hiIdx], limited to lags that can actually overlap.
    const SamplePos coarseLo = std::max(floorDiv(loIdx, d), -(SamplePos) refD.size() + 1);
    const SamplePos coarseHi = std::min(floorDiv(hiIdx, d) + 1, (SamplePos) tgtD.size() - 1);
    if (coarseLo > coarseHi)
    {
        error = kSilent;
        return std::nullopt;
    }
    auto phatAt = [&](SamplePos l) {
        const auto idx = (size_t) (l >= 0 ? l : (SamplePos) N + l);
        return std::abs(X[idx]);
    };
    SamplePos coarse = coarseLo;
    float peak = -1.0f;
    if (! inBlocks(coarseLo, coarseHi + 1, stage(0.6f, 0.62f), [&](SamplePos lb, SamplePos le) {
            for (SamplePos l = lb; l < le; ++l)
                if (const float v = phatAt(l); v > peak)
                {
                    peak = v;
                    coarse = l;
                }
        }))
        return cancelled();

    // Every comparison with NaN is false, so a broken correlation leaves `peak` at its start value: no result,
    // rather than the first lag of the range with a huge confidence.
    if (! (peak > 0.0f) || ! std::isfinite(peak))
    {
        error = kInvalid;
        return std::nullopt;
    }

    const auto exclusion = (SamplePos) std::ceil(0.005 * sampleRate / d);
    float second = 0.0f;
    if (! inBlocks(coarseLo, coarseHi + 1, stage(0.62f, 0.65f), [&](SamplePos lb, SamplePos le) {
            for (SamplePos l = lb; l < le; ++l)
                if (std::abs(l - coarse) > exclusion)
                    if (const float v = phatAt(l); v > second) // NaN-safe: a NaN never becomes the runner-up
                        second = v;
        }))
        return cancelled();
    const float confidence = second > 0.0f ? std::min(peak / second, kNoSecondPeakConfidence) : kNoSecondPeakConfidence;

    if (!report(0.65f))
        return cancelled();

    // ---- Fine pass: normalised cross-correlation at full rate within ±2d of the coarse lag -------------
    const SamplePos fineLo = std::max(coarse * d - 2 * d, loIdx);
    const SamplePos fineHi = std::min(coarse * d + 2 * d, hiIdx);
    SamplePos best = std::clamp(coarse * d, loIdx, hiIdx);
    double bestCorr = 0.0;
    const SamplePos count = fineHi - fineLo + 1;
    for (SamplePos l = fineLo; l <= fineHi; ++l)
    {
        const double c = normalisedCorrelation(reference, target, l);
        if (std::abs(c) > std::abs(bestCorr))
        {
            bestCorr = c;
            best = l;
        }
        const auto done = l - fineLo + 1;
        if (!report(0.65f + 0.35f * (float) done / (float) count))
            return cancelled();
    }
    if (count <= 0 && !report(1.0f))
        return cancelled();

    return OffsetResult { best + targetIndexOffset, confidence, bestCorr < 0.0 };
}

std::optional<OffsetResult> findOffset(const std::vector<float>& reference, const std::vector<float>& target,
                                       SamplePos maxLag, double sampleRate,
                                       const std::function<bool(float)>& progress, juce::String& error)
{
    return findOffset(reference, target, 0, maxLag, sampleRate, progress, error);
}
} // namespace aligna
