#pragma once
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace aligna
{
using SamplePos = std::int64_t;

struct SourceInfo
{
    juce::String id, path;
    SamplePos length = 0;
    int numChannels = 0;
    bool missing = false;
};

struct Clip
{
    juce::String id, sourceId;
    SamplePos start = 0, sourceOffset = 0, length = 0;
    float gainDb = 0;
    SamplePos fadeIn = 0, fadeOut = 0;
    // Output length / source length. Positions above (sourceOffset, length, fades) are in stretched samples.
    double stretch = 1.0;
    double sourceBpm = 0.0; // 0 = no tempo; otherwise stretch follows project bpm
};

constexpr double kMinStretch = 0.25, kMaxStretch = 4.0;

inline bool nearlyEqual(double a, double b) { return std::abs(a - b) < 1e-12; }

struct Track
{
    juce::String id, name;
    float volumeDb = 0, pan = 0;
    bool mute = false, solo = false;
    std::vector<Clip> clips;
};

struct Project
{
    double sampleRate = 48000, bpm = 120;
    int beatsPerBar = 4, beatUnit = 4;
    std::vector<SourceInfo> sources;
    std::vector<Track> tracks;
};

inline juce::String newId() { return juce::Uuid().toString(); }

inline SamplePos projectEnd(const Project& p)
{
    SamplePos e = 0;
    for (auto& t : p.tracks)
        for (auto& c : t.clips)
            e = std::max(e, c.start + c.length);
    return e;
}

inline const SourceInfo* findSource(const Project& p, const juce::String& id)
{
    for (auto& s : p.sources)
        if (s.id == id)
            return &s;
    return nullptr;
}

// Key of the buffer a clip plays: the source itself at ratio 1, else the source plus the exact ratio.
inline juce::String bufferKey(const Clip& c)
{
    if (nearlyEqual(c.stretch, 1.0))
        return c.sourceId;
    return c.sourceId + "@" + juce::String::formatted("%.17g", c.stretch);
}

inline SamplePos stretchedSourceLength(const Project& p, const Clip& c)
{
    auto* s = findSource(p, c.sourceId);
    return s == nullptr ? 0 : static_cast<SamplePos>(std::llround(static_cast<double>(s->length) * c.stretch));
}

// Largest ratio that keeps the stretched source within 2 hours at the project rate.
inline double maxStretchFor(const Project& p, const Clip& c)
{
    auto* s = findSource(p, c.sourceId);
    if (s == nullptr || s->length <= 0)
        return kMaxStretch;
    return std::min(kMaxStretch, 2.0 * 3600.0 * p.sampleRate / static_cast<double>(s->length));
}
} // namespace aligna
