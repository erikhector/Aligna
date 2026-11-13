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
constexpr double kSilenceRms = 1e-5;
constexpr float kNoSecondPeakConfidence = 1000.0f;

double rms(const std::vector<float>& v)
{
    if (v.empty())
        return 0.0;
    double s = 0.0;
    for (float x : v)
        s += (double) x * (double) x;
    return std::sqrt(s / (double) v.size());
}

// Moving average of length d, then keep every d-th sample.
std::vector<float> decimate(const std::vector<float>& x, int d)
{
    if (d <= 1)
        return x;
    std::vector<float> out((x.size() + (size_t) d - 1) / (size_t) d);
    for (size_t k = 0; k < out.size(); ++k)
    {
        double s = 0.0;
        const size_t b = k * (size_t) d, e = std::min(x.size(), b + (size_t) d);
        for (size_t i = b; i < e; ++i)
            s += x[i];
        out[k] = (float) (s / d);
    }
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
    auto cancelled = [&]() -> std::optional<OffsetResult> {
        error = kCancelled;
        return std::nullopt;
    };

    if (!report(0.0f))
        return cancelled();

    if (rms(reference) < kSilenceRms || rms(target) < kSilenceRms)
    {
        error = kSilent;
        return std::nullopt;
    }

    maxLag = std::max<SamplePos>(0, maxLag);
    // Index lag l (target[i] ≈ ±reference[i - l]) relates to the reference-position lag L by L = l + offset.
    const SamplePos loIdx = -maxLag - targetIndexOffset, hiIdx = maxLag - targetIndexOffset;

    // ---- Coarse pass: GCC-PHAT at ~8 kHz -------------------------------------------------------------
    const int d = std::max(1, (int) std::floor(sampleRate / 8000.0));
    const auto refD = decimate(reference, d);
    const auto tgtD = decimate(target, d);
    if (!report(0.05f))
        return cancelled();

    const int order = ceilLog2(refD.size() + tgtD.size());
    const size_t N = (size_t) 1 << order;
    juce::dsp::FFT fft(order);

    std::vector<float> X(2 * N, 0.0f), Y(2 * N, 0.0f);
    std::copy(tgtD.begin(), tgtD.end(), X.begin());
    std::copy(refD.begin(), refD.end(), Y.begin());
    fft.performRealOnlyForwardTransform(X.data(), true);
    if (!report(0.2f))
        return cancelled();
    fft.performRealOnlyForwardTransform(Y.data(), true);
    if (!report(0.35f))
        return cancelled();

    // R = X·conj(Y) / |X·conj(Y)|; its inverse peaks at the index lag (in decimated samples).
    for (size_t k = 0; k <= N / 2; ++k)
    {
        const std::complex<float> x(X[2 * k], X[2 * k + 1]), y(Y[2 * k], Y[2 * k + 1]);
        const auto c = x * std::conj(y);
        const auto r = c / std::max(std::abs(c), 1e-12f);
        X[2 * k] = r.real();
        X[2 * k + 1] = r.imag();
    }
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
    for (SamplePos l = coarseLo; l <= coarseHi; ++l)
        if (const float v = phatAt(l); v > peak)
        {
            peak = v;
            coarse = l;
        }

    const auto exclusion = (SamplePos) std::ceil(0.005 * sampleRate / d);
    float second = 0.0f;
    for (SamplePos l = coarseLo; l <= coarseHi; ++l)
        if (std::abs(l - coarse) > exclusion)
            second = std::max(second, phatAt(l));
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
