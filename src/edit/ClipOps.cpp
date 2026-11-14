#include "edit/ClipOps.h"
#include <cmath>

namespace aligna
{
namespace
{
void clampFades(Clip& c)
{
    const SamplePos m = c.length / 2;
    c.fadeIn = std::clamp<SamplePos>(c.fadeIn, 0, m);
    c.fadeOut = std::clamp<SamplePos>(c.fadeOut, 0, m);
}

// Rescales a clip to a new ratio (already clamped); the start never moves.
void applyStretch(const Project& p, Clip& c, double r2)
{
    const double k = r2 / c.stretch;
    auto scale = [k](SamplePos v) { return static_cast<SamplePos>(std::llround(static_cast<double>(v) * k)); };
    c.sourceOffset = scale(c.sourceOffset);
    c.length = std::max<SamplePos>(1, scale(c.length));
    c.fadeIn = scale(c.fadeIn);
    c.fadeOut = scale(c.fadeOut);
    c.stretch = r2;
    if (findSource(p, c.sourceId) != nullptr)
        c.length = std::min(c.length, std::max<SamplePos>(1, stretchedSourceLength(p, c) - c.sourceOffset));
    clampFades(c);
}

double clampRatio(const Project& p, const Clip& c, double r)
{
    return std::clamp(r, kMinStretch, std::max(kMinStretch, maxStretchFor(p, c)));
}
} // namespace

Clip* findClip(Project& p, const juce::String& clipId, int* trackIndex)
{
    for (size_t t = 0; t < p.tracks.size(); ++t)
        for (auto& c : p.tracks[t].clips)
            if (c.id == clipId)
            {
                if (trackIndex != nullptr)
                    *trackIndex = (int) t;
                return &c;
            }
    return nullptr;
}

juce::String addTrackWithClip(Project& p, const SourceInfo& src, const juce::String& trackName)
{
    if (findSource(p, src.id) == nullptr)
        p.sources.push_back(src);
    Track t;
    t.id = newId();
    t.name = trackName;
    Clip c;
    c.id = newId();
    c.sourceId = src.id;
    c.start = 0;
    c.sourceOffset = 0;
    c.length = src.length; // stretch is 1.0, so this is the stretched source length
    t.clips.push_back(c);
    p.tracks.push_back(std::move(t));
    return c.id;
}

juce::String addTakeTrack(Project& p, const SourceInfo& src, const juce::String& trackName, const TakePlacement& tp)
{
    auto id = addTrackWithClip(p, src, trackName);
    auto* c = findClip(p, id);
    c->start = tp.start;
    c->sourceOffset = tp.sourceOffset;
    c->length = tp.length;
    return id;
}

bool moveClip(Project& p, const juce::String& clipId, SamplePos newStart, int newTrackIndex)
{
    if (newTrackIndex < 0 || newTrackIndex >= (int) p.tracks.size())
        return false;
    int ti = -1;
    auto* c = findClip(p, clipId, &ti);
    if (c == nullptr)
        return false;
    Clip copy = *c;
    copy.start = std::max<SamplePos>(0, newStart);
    if (ti == newTrackIndex)
    {
        *c = copy;
        return true;
    }
    auto& from = p.tracks[(size_t) ti].clips;
    from.erase(from.begin() + (c - from.data()));
    p.tracks[(size_t) newTrackIndex].clips.push_back(copy);
    return true;
}

bool trimClipStart(Project& p, const juce::String& clipId, SamplePos newStart)
{
    auto* c = findClip(p, clipId);
    if (c == nullptr || findSource(p, c->sourceId) == nullptr)
        return false;
    const SamplePos end = c->start + c->length;
    const SamplePos lo = c->start - c->sourceOffset; // keeps sourceOffset >= 0
    const SamplePos hi = end - 1;                    // keeps length >= 1
    const SamplePos ns = std::clamp(newStart, lo, hi);
    const SamplePos d = ns - c->start;
    c->start = ns;
    c->sourceOffset += d;
    c->length -= d;
    clampFades(*c);
    return true;
}

bool trimClipEnd(Project& p, const juce::String& clipId, SamplePos newEnd)
{
    auto* c = findClip(p, clipId);
    if (c == nullptr)
        return false;
    auto* s = findSource(p, c->sourceId);
    if (s == nullptr)
        return false;
    const SamplePos maxLen = std::max<SamplePos>(1, stretchedSourceLength(p, *c) - c->sourceOffset);
    c->length = std::clamp<SamplePos>(newEnd - c->start, 1, maxLen);
    clampFades(*c);
    return true;
}

std::optional<juce::String> splitClip(Project& p, const juce::String& clipId, SamplePos at)
{
    int ti = -1;
    auto* c = findClip(p, clipId, &ti);
    if (c == nullptr || at <= c->start || at >= c->start + c->length)
        return std::nullopt;
    Clip right = *c;
    right.id = newId();
    const SamplePos d = at - c->start;
    right.start = at;
    right.sourceOffset = c->sourceOffset + d;
    right.length = c->length - d;
    right.fadeIn = 0;
    c->length = d;
    c->fadeOut = 0;
    clampFades(*c);
    clampFades(right);
    auto id = right.id;
    auto& v = p.tracks[(size_t) ti].clips;
    v.push_back(std::move(right)); // may invalidate c; not used after
    return id;
}

bool deleteClip(Project& p, const juce::String& clipId)
{
    for (auto& t : p.tracks)
        for (auto it = t.clips.begin(); it != t.clips.end(); ++it)
            if (it->id == clipId)
            {
                t.clips.erase(it);
                return true;
            }
    return false;
}

bool setClipGain(Project& p, const juce::String& clipId, float gainDb)
{
    auto* c = findClip(p, clipId);
    if (c == nullptr)
        return false;
    c->gainDb = std::clamp(gainDb, -60.0f, 12.0f);
    return true;
}

bool setClipFades(Project& p, const juce::String& clipId, SamplePos fadeIn, SamplePos fadeOut)
{
    auto* c = findClip(p, clipId);
    if (c == nullptr)
        return false;
    c->fadeIn = fadeIn;
    c->fadeOut = fadeOut;
    clampFades(*c);
    return true;
}

bool setClipStretch(Project& p, const juce::String& clipId, double newStretch)
{
    auto* c = findClip(p, clipId);
    if (c == nullptr || !std::isfinite(newStretch))
        return false;
    const double r = clampRatio(p, *c, newStretch);
    if (nearlyEqual(r, c->stretch) && c->sourceBpm <= 0.0)
        return false;
    if (!nearlyEqual(r, c->stretch))
        applyStretch(p, *c, r);
    c->sourceBpm = 0.0;
    return true;
}

bool setClipTempo(Project& p, const juce::String& clipId, double sourceBpm)
{
    auto* c = findClip(p, clipId);
    if (c == nullptr || !std::isfinite(sourceBpm))
        return false;
    if (sourceBpm <= 0.0)
    {
        if (c->sourceBpm <= 0.0)
            return false;
        c->sourceBpm = 0.0;
        return true;
    }
    const double bpm = std::clamp(sourceBpm, 20.0, 400.0);
    const double r = clampRatio(p, *c, bpm / p.bpm);
    if (nearlyEqual(r, c->stretch) && nearlyEqual(bpm, c->sourceBpm))
        return false;
    if (!nearlyEqual(r, c->stretch))
        applyStretch(p, *c, r);
    c->sourceBpm = bpm;
    return true;
}

bool setProjectBpm(Project& p, double bpm)
{
    if (!std::isfinite(bpm))
        return false;
    bpm = std::clamp(bpm, 20.0, 400.0);
    if (nearlyEqual(bpm, p.bpm))
        return false;
    p.bpm = bpm;
    for (auto& t : p.tracks)
        for (auto& c : t.clips)
            if (c.sourceBpm > 0.0)
            {
                const double r = clampRatio(p, c, c.sourceBpm / bpm);
                if (!nearlyEqual(r, c.stretch))
                    applyStretch(p, c, r);
            }
    return true;
}

bool removeTrack(Project& p, int trackIndex)
{
    if (trackIndex < 0 || trackIndex >= (int) p.tracks.size())
        return false;
    p.tracks.erase(p.tracks.begin() + trackIndex);
    return true;
}

bool removeTrackById(Project& p, const juce::String& trackId)
{
    for (size_t i = 0; i < p.tracks.size(); ++i)
        if (p.tracks[i].id == trackId)
            return removeTrack(p, (int) i);
    return false;
}
} // namespace aligna
