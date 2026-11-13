#pragma once
#include <juce_core/juce_core.h>
#include <algorithm>
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
};

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
} // namespace aligna
